/*
 * Memento Memory Allocator Library
 *
 * A high-performance, multi-allocator memory management library.
 * Alloc/free on the owning thread is lock-free and atomic-free; frees from
 * OTHER threads are safe and lock-free via a per-heap atomic MPSC stack that
 * the owner drains (memento_thread_heap_flush) on its own schedule.
 *
 * Features:
 * - Thread Heap: thread-local size-class caching; cross-thread free is safe
 * - Pool: Fixed-size object pools
 * - Arena: Bump allocator with power-of-2 growth (aligned allocation)
 * - Stack: LIFO scope-based allocator
 * - Slab: Multi-size object caching
 *
 * Platforms: POSIX (Linux/macOS/BSD) and Windows (MSVC / MinGW / Clang-CL)
 *
 * Threading contract:
 * - memento_thread_heap_alloc / realloc / flush: owning thread only
 * - memento_thread_heap_free: ANY thread (exact size required)
 * - When a thread exits, its heap is flushed and retired (TLS destructor);
 *   frees that target a retired heap park on its foreign stack and are
 *   reclaimed by memento_shutdown.
 * - memento_shutdown must happen-after every free targeting any heap
 *   (i.e. join your threads first).
 *
 * Usage (C):
 *   #define MEMENTO_IMPLEMENTATION
 *   #include "memento.h"
 *
 *   int main() {
 *       memento_init();
 *       memento_thread_heap_t* heap = memento_thread_heap_get();
 *       void* ptr = memento_thread_heap_alloc(heap, 1024);
 *       memento_thread_heap_free(heap, ptr, 1024);
 *       memento_shutdown();
 *       return 0;
 *   }
 *
 * License: MIT
 */

#ifndef MEMENTO_H
#define MEMENTO_H

/* Version macros for compile-time checking */
#define MEMENTO_VERSION_MAJOR 2
#define MEMENTO_VERSION_MINOR 2
#define MEMENTO_VERSION_PATCH 1
#define MEMENTO_VERSION_STRING "2.2.1"
#define MEMENTO_VERSION ((MEMENTO_VERSION_MAJOR << 16) | \
                         (MEMENTO_VERSION_MINOR << 8) | \
                         MEMENTO_VERSION_PATCH)

/* ============================================================================
 * Standard Headers
 * ============================================================================ */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>

/* Atomics for the per-heap foreign-free stack.
 * Included BEFORE any extern "C" so C++ <atomic> is not pulled into C linkage
 * (memento.hpp must NOT wrap this header in extern "C" either). */
#if defined(__cplusplus)
    #include <atomic>
#elif defined(_MSC_VER) && !defined(__clang__)
    /* MSVC C: Interlocked* APIs; windows.h pulled in under IMPLEMENTATION */
#else
    #include <stdatomic.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration and Platform Detection
 * ============================================================================ */

#if defined(_WIN32) || defined(__WIN32__) || defined(_WIN64)
    #define MEMENTO_PLATFORM_WINDOWS 1
    #define MEMENTO_PLATFORM_POSIX 0
#else
    #define MEMENTO_PLATFORM_WINDOWS 0
    #define MEMENTO_PLATFORM_POSIX 1
#endif

#if defined(_MSC_VER) && !defined(__clang__)
    #define MEMENTO_TLS __declspec(thread)
    #define MEMENTO_FORCE_INLINE static __forceinline
    #define MEMENTO_NOINLINE __declspec(noinline)
    #define MEMENTO_ALIGNED(x) __declspec(align(x))
#elif defined(_MSC_VER) && defined(__clang__)
    /* clang-cl */
    #define MEMENTO_TLS __declspec(thread)
    #define MEMENTO_FORCE_INLINE static inline __attribute__((always_inline))
    #define MEMENTO_NOINLINE __attribute__((noinline))
    #define MEMENTO_ALIGNED(x) __declspec(align(x))
#else
    #define MEMENTO_TLS __thread
    #define MEMENTO_FORCE_INLINE static inline __attribute__((always_inline))
    #define MEMENTO_NOINLINE __attribute__((noinline))
    #define MEMENTO_ALIGNED(x) __attribute__((aligned(x)))
#endif

/* Cache line size (common values: 64 for x86/ARM, 128 for POWER) */
#ifndef MEMENTO_CACHE_LINE_SIZE
    #define MEMENTO_CACHE_LINE_SIZE 64
#endif

/* Threshold: allocations >= this use OS virtual memory (mmap/VirtualAlloc) */
#ifndef MEMENTO_MMAP_THRESHOLD
    #define MEMENTO_MMAP_THRESHOLD (64 * 1024)
#endif

/* Max size class (bytes); larger goes through the large-object path */
#ifndef MEMENTO_MAX_SIZE_CLASS
    #define MEMENTO_MAX_SIZE_CLASS 8192
#endif

/* Thread-local span size for size-class refill (bump-carved blocks) */
#ifndef MEMENTO_SPAN_SIZE
    #define MEMENTO_SPAN_SIZE (2u * 1024u * 1024u)
#endif

/* How many blocks to carve into the freelist on a size-class cache miss */
#ifndef MEMENTO_REFILL_BATCH
    #define MEMENTO_REFILL_BATCH 32
#endif

/* Large-object cache: exact-size reuse before mmap/munmap */
#ifndef MEMENTO_LARGE_CACHE_SLOTS
    #define MEMENTO_LARGE_CACHE_SLOTS 8
#endif
/* Do not cache large objects bigger than this (user size) */
#ifndef MEMENTO_LARGE_CACHE_MAX_SIZE
    #define MEMENTO_LARGE_CACHE_MAX_SIZE (4u * 1024u * 1024u)
#endif

/* Foreign free flush batch size (also accepted as MEMENTO_FOREIGN_BATCH) */
#ifdef MEMENTO_FOREIGN_BATCH
    #ifndef MEMENTO_FLUSH_BATCH
        #define MEMENTO_FLUSH_BATCH MEMENTO_FOREIGN_BATCH
    #endif
#endif
#ifndef MEMENTO_FLUSH_BATCH
    #define MEMENTO_FLUSH_BATCH 64
#endif
#ifndef MEMENTO_FOREIGN_BATCH
    #define MEMENTO_FOREIGN_BATCH MEMENTO_FLUSH_BATCH
#endif

/* Opt-in debug: freelist canaries + double-free checks (not on by default) */
#ifndef MEMENTO_DEBUG
    #define MEMENTO_DEBUG 0
#endif

/* NUMA first-touch / node tagging (disable with -DMEMENTO_ENABLE_NUMA=0) */
#ifndef MEMENTO_ENABLE_NUMA
    #define MEMENTO_ENABLE_NUMA 1
#endif

/* Transparent huge pages via madvise (POSIX); no-op on Windows */
#ifndef MEMENTO_ENABLE_THP
    #define MEMENTO_ENABLE_THP 1
#endif

/* AddressSanitizer freelist poison/unpoison (auto if ASan is active) */
#ifndef MEMENTO_ENABLE_ASAN
    #if defined(__SANITIZE_ADDRESS__)
        #define MEMENTO_ENABLE_ASAN 1
    #elif defined(__has_feature)
        #if __has_feature(address_sanitizer)
            #define MEMENTO_ENABLE_ASAN 1
        #else
            #define MEMENTO_ENABLE_ASAN 0
        #endif
    #else
        #define MEMENTO_ENABLE_ASAN 0
    #endif
#endif

/* Branch prediction hints */
#if defined(__GNUC__) || defined(__clang__)
    #define MEMENTO_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define MEMENTO_UNLIKELY(x) __builtin_expect(!!(x), 0)
    #define MEMENTO_PREFETCH(addr) __builtin_prefetch((addr), 0, 1)
#else
    #define MEMENTO_LIKELY(x)   (x)
    #define MEMENTO_UNLIKELY(x) (x)
    #define MEMENTO_PREFETCH(addr) ((void)0)
#endif

/* restrict: standard C, __restrict for C++ / MSVC */
#if defined(__cplusplus)
    #define MEMENTO_RESTRICT __restrict
#elif defined(_MSC_VER)
    #define MEMENTO_RESTRICT __restrict
#else
    #define MEMENTO_RESTRICT restrict
#endif

/* Atomic pointer - C11 _Atomic / std::atomic / MSVC Interlocked.
 * Only used for the per-heap foreign-free MPSC stack head. */
#if defined(__cplusplus)
    typedef std::atomic<void*> memento_atomic_ptr_t;
    #define memento_atomic_ptr_load_relaxed(p) \
        ((p)->load(std::memory_order_relaxed))
    #define memento_atomic_ptr_exchange_acq(p, v) \
        ((p)->exchange((v), std::memory_order_acquire))
    #define memento_atomic_ptr_cas_release(p, expected, desired) \
        ((p)->compare_exchange_weak((expected), (desired), \
                                    std::memory_order_release, \
                                    std::memory_order_relaxed))
    #define memento_atomic_ptr_store_relaxed(p, v) \
        ((p)->store((v), std::memory_order_relaxed))
#elif defined(_MSC_VER) && !defined(__clang__)
    /* Defined as opaque void*; Interlocked ops in IMPLEMENTATION block */
    typedef void* memento_atomic_ptr_t;
#else
    typedef _Atomic(void*) memento_atomic_ptr_t;
    #define memento_atomic_ptr_load_relaxed(p) \
        atomic_load_explicit((p), memory_order_relaxed)
    #define memento_atomic_ptr_exchange_acq(p, v) \
        atomic_exchange_explicit((p), (v), memory_order_acquire)
    #define memento_atomic_ptr_cas_release(p, expected, desired) \
        atomic_compare_exchange_weak_explicit((p), &(expected), (desired), \
                                              memory_order_release, \
                                              memory_order_relaxed)
    #define memento_atomic_ptr_store_relaxed(p, v) \
        atomic_store_explicit((p), (v), memory_order_relaxed)
#endif

/* ============================================================================
 * Public API
 * ============================================================================ */

typedef struct memento_thread_heap_s memento_thread_heap_t;
typedef struct memento_pool_s memento_pool_t;
typedef struct memento_arena_s memento_arena_t;
typedef struct memento_stack_s memento_stack_t;
typedef struct memento_slab_s memento_slab_t;

/* Size classes (24): fine grain in 32–512B, coarser above.
 * 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448,
 * 512, 640, 768, 896, 1024, 1280, 1536, 2048, 4096, 8192 */
#define MEMENTO_SIZE_CLASS_COUNT 24

/* Thread-local heap statistics (for debugging/monitoring) */
typedef struct {
    size_t alloc_count;
    size_t free_count;
    size_t bytes_allocated;
    size_t bytes_freed;
    size_t foreign_free_count;  /* Freed by other threads */
} memento_heap_stats_t;

/* ============================================================================
 * Version API
 * ============================================================================ */

static inline const char* memento_version_string(void) {
    return MEMENTO_VERSION_STRING;
}

static inline unsigned int memento_version_number(void) {
    return MEMENTO_VERSION;
}

static inline int memento_version_check(int major, int minor, int patch) {
    return MEMENTO_VERSION >= ((major << 16) | (minor << 8) | patch);
}

/* ============================================================================
 * Thread Heap API - Non-locking, thread-local caching
 * ============================================================================ */

bool memento_init(void);
void memento_shutdown(void);

/* Get thread-local heap (creates on first call, cached in TLS) */
memento_thread_heap_t* memento_thread_heap_get(void);

/* Allocate from thread-local heap (owning thread only, non-locking).
 * memento_thread_heap_free may be called from ANY thread: foreign frees are
 * pushed onto the heap's lock-free MPSC stack and reclaimed by the owner.
 * The exact allocation size must be passed to free. */
void* memento_thread_heap_alloc(memento_thread_heap_t* heap, size_t size);
void memento_thread_heap_free(memento_thread_heap_t* heap, void* ptr, size_t size);
void* memento_thread_heap_realloc(memento_thread_heap_t* heap, void* ptr,
                                   size_t old_size, size_t new_size);

/* Drain pending foreign deallocations (owning thread only; cheap when empty -
 * a single relaxed load). Call from scheduler idle paths. */
void memento_thread_heap_flush(memento_thread_heap_t* heap);

void memento_thread_heap_stats(memento_thread_heap_t* heap, memento_heap_stats_t* stats);

/* NUMA node this heap first-touched spans on, or -1 if unknown / disabled */
int memento_thread_heap_numa_node(const memento_thread_heap_t* heap);

/* ============================================================================
 * Pool Allocator API - Fixed-size object pools
 * ============================================================================ */

memento_pool_t* memento_pool_create(size_t object_size, size_t capacity,
                                     memento_thread_heap_t* heap);
void memento_pool_destroy(memento_pool_t* pool);

void* memento_pool_alloc(memento_pool_t* pool);
void memento_pool_free(memento_pool_t* pool, void* ptr);

/* ============================================================================
 * Arena Allocator API - Bump allocator with power-of-2 growth
 * ============================================================================ */

memento_arena_t* memento_arena_create(size_t initial_capacity,
                                       memento_thread_heap_t* heap);
void memento_arena_destroy(memento_arena_t* arena);

/* alignment must be a power of two (or 0, treated as 1) */
void* memento_arena_alloc(memento_arena_t* arena, size_t size, size_t alignment);

/* Save/restore for temporary allocations.
 * Restore is only valid if no newer block was allocated after the save
 * (i.e. the arena did not grow). After growth, restore still rewinds the
 * recorded block's top; excess blocks remain until reset/destroy. */
typedef struct {
    void* saved_block;   /* memento_arena_block_t* at save time */
    void* saved_top;
    size_t saved_used;
    size_t saved_capacity;
} memento_arena_save_t;

memento_arena_save_t memento_arena_save(memento_arena_t* arena);
void memento_arena_restore(memento_arena_t* arena, memento_arena_save_t* save);

/* Reset arena to empty (releases grown blocks back to the thread heap) */
void memento_arena_reset(memento_arena_t* arena);

size_t memento_arena_used(const memento_arena_t* arena);
size_t memento_arena_capacity(const memento_arena_t* arena);

/* ============================================================================
 * Stack Allocator API - LIFO scope-based allocation
 * ============================================================================ */

memento_stack_t* memento_stack_create(size_t capacity, memento_thread_heap_t* heap);
void memento_stack_destroy(memento_stack_t* stack);

/* alignment must be a power of two (or 0, treated as 1) */
void* memento_stack_push(memento_stack_t* stack, size_t size, size_t alignment);
/* Rolls top back to ptr (must be the most recent live push, LIFO). */
void memento_stack_pop(memento_stack_t* stack, void* ptr);

typedef size_t memento_stack_marker_t;

memento_stack_marker_t memento_stack_marker(memento_stack_t* stack);
void memento_stack_pop_to_marker(memento_stack_t* stack, memento_stack_marker_t marker);

void memento_stack_reset(memento_stack_t* stack);

/* ============================================================================
 * Slab Allocator API - Multi-size object caching
 * ============================================================================ */

memento_slab_t* memento_slab_create(memento_thread_heap_t* heap);
void memento_slab_destroy(memento_slab_t* slab);

void* memento_slab_alloc(memento_slab_t* slab, size_t size);
void memento_slab_free(memento_slab_t* slab, void* ptr, size_t size);

#ifdef __cplusplus
}
#endif

/* ============================================================================
 * Implementation Section
 *
 * Include this in exactly ONE source file:
 *   #define MEMENTO_IMPLEMENTATION
 *   #include "memento.h"
 * ============================================================================ */

#ifdef MEMENTO_IMPLEMENTATION

#ifdef __cplusplus
  #if defined(__clang__)
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wc99-extensions"
    #pragma clang diagnostic ignored "-Wc++20-designator"
    #pragma clang diagnostic ignored "-Wmissing-field-initializers"
  #elif defined(__GNUC__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wpedantic"
    #pragma GCC diagnostic ignored "-Wmissing-field-initializers"
  #endif
#endif

/* ============================================================================
 * Platform Layer
 * ============================================================================ */

#if MEMENTO_PLATFORM_WINDOWS
    /* Vista+ for FlsAlloc (fiber-local storage with destructor callbacks).
     * Must be set before windows.h. */
    /* 0x0601 = Win7+: GetCurrentProcessorNumberEx / GetNumaProcessorNodeEx */
    #if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0601)
        #undef _WIN32_WINNT
        #define _WIN32_WINNT 0x0601
    #endif
    #if !defined(WINVER) || (WINVER < 0x0601)
        #undef WINVER
        #define WINVER 0x0601
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <malloc.h> /* _aligned_malloc / _aligned_free */

    #ifndef MEMENTO_MALLOC
        #define MEMENTO_MALLOC(size) malloc(size)
    #endif
    #ifndef MEMENTO_FREE
        #define MEMENTO_FREE(ptr, size) ((void)(size), free(ptr))
    #endif
    #ifndef MEMENTO_ALIGNED_ALLOC
        #define MEMENTO_ALIGNED_ALLOC(align, size) _aligned_malloc((size), (align))
    #endif
    #ifndef MEMENTO_ALIGNED_FREE
        #define MEMENTO_ALIGNED_FREE(ptr) _aligned_free(ptr)
    #endif
    #ifndef MEMENTO_MMAP
        #define MEMENTO_MMAP(size) \
            VirtualAlloc(NULL, (SIZE_T)(size), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
    #endif
    #ifndef MEMENTO_MUNMAP
        #define MEMENTO_MUNMAP(ptr, size) \
            ((void)(size), VirtualFree((ptr), 0, MEM_RELEASE))
    #endif

    /* MSVC C Interlocked atomics (when not using stdatomic / C++) */
    #if defined(_MSC_VER) && !defined(__clang__) && !defined(__cplusplus)
        static __forceinline void* memento_atomic_ptr_load_relaxed(memento_atomic_ptr_t* p) {
            return *(void* volatile*)p;
        }
        static __forceinline void* memento_atomic_ptr_exchange_acq(memento_atomic_ptr_t* p, void* v) {
            return InterlockedExchangePointer((PVOID*)p, v);
        }
        static __forceinline int memento_atomic_ptr_cas_release(
                memento_atomic_ptr_t* p, void* expected, void* desired) {
            void* prev = InterlockedCompareExchangePointer((PVOID*)p, desired, expected);
            return prev == expected;
        }
        static __forceinline void memento_atomic_ptr_store_relaxed(
                memento_atomic_ptr_t* p, void* v) {
            *(void* volatile*)p = v;
        }
    #endif

    /* Registry lock */
    static CRITICAL_SECTION memento_registry_lock;
    static int memento_registry_lock_init = 0;

    static void memento_registry_lock_ensure(void) {
        if (!memento_registry_lock_init) {
            InitializeCriticalSection(&memento_registry_lock);
            memento_registry_lock_init = 1;
        }
    }
    #define MEMENTO_REGISTRY_LOCK()   do { memento_registry_lock_ensure(); EnterCriticalSection(&memento_registry_lock); } while (0)
    #define MEMENTO_REGISTRY_UNLOCK() LeaveCriticalSection(&memento_registry_lock)

    /* FLS for TLS destructor on thread exit */
    static DWORD memento_fls_index = FLS_OUT_OF_INDEXES;
    static int memento_fls_ready = 0;

#else /* POSIX */
    #include <sys/mman.h>
    #include <unistd.h>
    #include <pthread.h>
    #include <errno.h>
    #if defined(__linux__)
        #include <sys/syscall.h>
    #endif

    #ifndef MAP_ANONYMOUS
        #ifdef MAP_ANON
            #define MAP_ANONYMOUS MAP_ANON
        #else
            #define MAP_ANONYMOUS 0x20
        #endif
    #endif

    #ifndef MEMENTO_MALLOC
        #define MEMENTO_MALLOC(size) malloc(size)
    #endif
    #ifndef MEMENTO_FREE
        #define MEMENTO_FREE(ptr, size) ((void)(size), free(ptr))
    #endif
    #ifndef MEMENTO_ALIGNED_ALLOC
        /* aligned_alloc requires size multiple of alignment (C11) */
        #define MEMENTO_ALIGNED_ALLOC(align, size) \
            aligned_alloc((align), (((size) + (align) - 1) / (align) * (align)))
    #endif
    #ifndef MEMENTO_ALIGNED_FREE
        #define MEMENTO_ALIGNED_FREE(ptr) free(ptr)
    #endif
    #ifndef MEMENTO_MMAP
        #define MEMENTO_MMAP(size) \
            mmap(NULL, (size), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
    #endif
    #ifndef MEMENTO_MUNMAP
        #define MEMENTO_MUNMAP(ptr, size) munmap((ptr), (size))
    #endif

    static pthread_mutex_t memento_registry_lock = PTHREAD_MUTEX_INITIALIZER;
    #define MEMENTO_REGISTRY_LOCK()   pthread_mutex_lock(&memento_registry_lock)
    #define MEMENTO_REGISTRY_UNLOCK() pthread_mutex_unlock(&memento_registry_lock)

    static pthread_key_t memento_tls_key;
    static int memento_tls_key_created = 0;
#endif

/* ============================================================================
 * Size Classes - branchless LUT (16-byte buckets; classes use 16B steps)
 * ============================================================================ */

static const size_t memento_size_classes[MEMENTO_SIZE_CLASS_COUNT] = {
    32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320,
    384, 448, 512, 640, 768, 896, 1024, 1280, 1536, 2048, 4096, 8192
};

/* Index = (size + 15) >> 4 for size in 1..8192 (indices 1..512) */
static const uint8_t memento_sc_lut[513] = {
     0,  0,  0,  1,  2,  3,  4,  5,  6,  7,  7,  8,  8,  9,  9, 10,
    10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14,
    14, 15, 15, 15, 15, 15, 15, 15, 15, 16, 16, 16, 16, 16, 16, 16,
    16, 17, 17, 17, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 18,
    18, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    20, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
    21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
    21, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22,
    22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22,
    22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22,
    22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22,
    22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22,
    22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22,
    22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22,
    22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22,
    22, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23
};

MEMENTO_FORCE_INLINE size_t memento_size_class_for(size_t size) {
    if (MEMENTO_UNLIKELY(size == 0)) return 0;
    if (MEMENTO_UNLIKELY(size > MEMENTO_MAX_SIZE_CLASS)) {
        return MEMENTO_SIZE_CLASS_COUNT - 1;
    }
    /* 16-byte bucket index; table covers 1..8192 */
    return (size_t)memento_sc_lut[(size + 15) >> 4];
}

MEMENTO_FORCE_INLINE size_t memento_size_class_to_size(size_t sc) {
    return (sc < MEMENTO_SIZE_CLASS_COUNT)
        ? memento_size_classes[sc]
        : MEMENTO_MAX_SIZE_CLASS;
}

MEMENTO_FORCE_INLINE bool memento_is_power_of_two(size_t x) {
    return x != 0 && (x & (x - 1)) == 0;
}

MEMENTO_FORCE_INLINE size_t memento_align_up(size_t size, size_t alignment) {
    /* alignment must be power of two and non-zero */
    return (size + alignment - 1) & ~(alignment - 1);
}

MEMENTO_FORCE_INLINE size_t memento_align_down(size_t size, size_t alignment) {
    return size & ~(alignment - 1);
}

/* Saturating multiply: returns true on overflow */
MEMENTO_FORCE_INLINE bool memento_mul_overflow(size_t a, size_t b, size_t* out) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, out);
#else
    if (a != 0 && b > SIZE_MAX / a) return true;
    *out = a * b;
    return false;
#endif
}

/* ============================================================================
 * Internal Thread Cache
 * ============================================================================ */

typedef struct memento_cache_node_s {
    struct memento_cache_node_s* next;
} memento_cache_node_t;

/* One cache line per size class — eliminates false sharing when multiple
 * threads allocate different classes (and keeps hot free-list head alone). */
typedef struct {
    void* head;
    uint32_t count;
    uint32_t limit;
    uint8_t _pad[
        (MEMENTO_CACHE_LINE_SIZE > (sizeof(void*) + sizeof(uint32_t) * 2))
            ? (MEMENTO_CACHE_LINE_SIZE - sizeof(void*) - sizeof(uint32_t) * 2)
            : 1
    ];
} memento_size_class_cache_t;

/* Node written into the first bytes of a foreign-freed block. The smallest
 * size class is 32 bytes, so every block can hold it. */
typedef struct memento_foreign_node_s {
    struct memento_foreign_node_s* next;
    size_t size;
} memento_foreign_node_t;

/* 2 MiB OS span: bump-carved into size-class blocks (thread-local) */
typedef struct memento_span_s {
    struct memento_span_s* next;
    size_t used;
    size_t capacity;
    /* Flexible data region starts after header; whole span is MEMENTO_SPAN_SIZE */
    char data[1];
} memento_span_t;

#define MEMENTO_SPAN_HEADER offsetof(memento_span_t, data)

/* Exact-size large-object cache slot (> MEMENTO_MAX_SIZE_CLASS) */
typedef struct {
    void* ptr;    /* user pointer (past size header) */
    size_t size;  /* user size */
} memento_large_slot_t;

struct memento_thread_heap_s {
    /* Size-class free lists: each entry is its own cache line */
    MEMENTO_ALIGNED(MEMENTO_CACHE_LINE_SIZE)
    memento_size_class_cache_t caches[MEMENTO_SIZE_CLASS_COUNT];

    /* Foreign deallocation stack (MPSC) — isolated cache line */
    MEMENTO_ALIGNED(MEMENTO_CACHE_LINE_SIZE) memento_atomic_ptr_t foreign_head;
    char foreign_pad[MEMENTO_CACHE_LINE_SIZE - sizeof(memento_atomic_ptr_t)];

    /* Cold fields (owner thread only, rarely shared) */
    MEMENTO_ALIGNED(MEMENTO_CACHE_LINE_SIZE) memento_heap_stats_t stats;
    uint64_t thread_id;
    struct memento_thread_heap_s* registry_next;

    /* Span bump allocator for size classes */
    memento_span_t* span_current;
    memento_span_t* span_list;

    /* Large-object cache (exact size match) */
    memento_large_slot_t large_cache[MEMENTO_LARGE_CACHE_SLOTS];
    uint32_t large_count;

    /* NUMA node observed at heap create (-1 unknown) */
    int32_t numa_node;

    uint8_t initialized;
    uint8_t retired;
    uint8_t _pad[2];
};

/* ============================================================================
 * Thread-local Storage
 * ============================================================================ */

static MEMENTO_TLS memento_thread_heap_t* memento_tls_heap = NULL;
static memento_thread_heap_t* memento_heap_registry = NULL;
static int memento_initialized = 0;

/* --------------------------------------------------------------------------
 * Debug / sanitizer helpers (zero cost when disabled)
 * -------------------------------------------------------------------------- */

#if MEMENTO_ENABLE_ASAN
    #if defined(__has_include)
        #if __has_include(<sanitizer/asan_interface.h>)
            #include <sanitizer/asan_interface.h>
        #endif
    #endif
    #ifndef ASAN_POISON_MEMORY_REGION
        #define ASAN_POISON_MEMORY_REGION(addr, size)   ((void)0)
        #define ASAN_UNPOISON_MEMORY_REGION(addr, size) ((void)0)
    #endif
    #define MEMENTO_ASAN_POISON(p, n)   ASAN_POISON_MEMORY_REGION((p), (n))
    #define MEMENTO_ASAN_UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION((p), (n))
#else
    #define MEMENTO_ASAN_POISON(p, n)   do { (void)(p); (void)(n); } while (0)
    #define MEMENTO_ASAN_UNPOISON(p, n) do { (void)(p); (void)(n); } while (0)
#endif

#if MEMENTO_DEBUG
    #define MEMENTO_FREE_MAGIC  0xF4EEF4EEu
    #define MEMENTO_LIVE_MAGIC  0xA110C8EDu

    static void memento_debug_fail(const char* msg) {
        fputs("memento: ", stderr);
        fputs(msg, stderr);
        fputc('\n', stderr);
        abort();
    }

    /* Freelist node layout: [next ptr][uint32 magic] — min class 32B so OK */
    MEMENTO_FORCE_INLINE void memento_debug_on_free(void* ptr) {
        uint32_t* magic = (uint32_t*)((char*)ptr + sizeof(void*));
        if (MEMENTO_UNLIKELY(*magic == MEMENTO_FREE_MAGIC)) {
            memento_debug_fail("double-free detected");
        }
        *magic = MEMENTO_FREE_MAGIC;
    }

    MEMENTO_FORCE_INLINE void memento_debug_on_alloc(void* ptr) {
        uint32_t* magic = (uint32_t*)((char*)ptr + sizeof(void*));
        if (MEMENTO_UNLIKELY(*magic != MEMENTO_FREE_MAGIC)) {
            memento_debug_fail("freelist corruption (bad canary)");
        }
        *magic = MEMENTO_LIVE_MAGIC;
    }

    MEMENTO_FORCE_INLINE void memento_debug_mark_fresh(void* ptr) {
        uint32_t* magic = (uint32_t*)((char*)ptr + sizeof(void*));
        *magic = MEMENTO_LIVE_MAGIC;
    }

    MEMENTO_FORCE_INLINE void memento_debug_mark_free(void* ptr) {
        uint32_t* magic = (uint32_t*)((char*)ptr + sizeof(void*));
        *magic = MEMENTO_FREE_MAGIC;
    }
#else
    #define memento_debug_on_free(ptr)    ((void)0)
    #define memento_debug_on_alloc(ptr)   ((void)0)
    #define memento_debug_mark_fresh(ptr) ((void)0)
    #define memento_debug_mark_free(ptr)  ((void)0)
#endif

/* --------------------------------------------------------------------------
 * NUMA node query (no libnuma dependency)
 * -------------------------------------------------------------------------- */

static int memento_query_numa_node(void) {
#if !MEMENTO_ENABLE_NUMA
    return -1;
#elif MEMENTO_PLATFORM_WINDOWS
    {
        USHORT node = 0;
        PROCESSOR_NUMBER proc;
        GetCurrentProcessorNumberEx(&proc);
        if (GetNumaProcessorNodeEx(&proc, &node)) {
            return (int)node;
        }
        return -1;
    }
#elif defined(__linux__)
    {
        /* getcpu(2) via raw syscall — works even when glibc hides syscall()
         * behind _GNU_SOURCE / feature-test macros. */
        unsigned cpu = 0, node = 0;
        long ret = -1;
    #if defined(__NR_getcpu)
        #if defined(__x86_64__)
        {
            register long r10 __asm__("r10") = 0;
            __asm__ volatile("syscall"
                : "=a"(ret)
                : "a"((long)__NR_getcpu), "D"(&cpu), "S"(&node), "d"((unsigned long)0), "r"(r10)
                : "rcx", "r11", "memory");
        }
        #elif defined(__aarch64__)
        {
            register long x8 __asm__("x8") = (long)__NR_getcpu;
            register long x0 __asm__("x0") = (long)(uintptr_t)&cpu;
            register long x1 __asm__("x1") = (long)(uintptr_t)&node;
            register long x2 __asm__("x2") = 0;
            __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
            ret = x0;
        }
        #endif
    #endif
        if (ret == 0) {
            (void)cpu;
            return (int)node;
        }
        return -1;
    }
#else
    return -1;
#endif
}

static uint64_t memento_get_thread_id(void) {
#if MEMENTO_PLATFORM_WINDOWS
    return (uint64_t)GetCurrentThreadId();
#elif defined(__linux__)
    return (uint64_t)pthread_self();
#else
    return (uint64_t)(uintptr_t)pthread_self();
#endif
}

static void memento_heap_init(memento_thread_heap_t* heap) {
    memset((void*)heap, 0, sizeof(memento_thread_heap_t));
    for (int i = 0; i < MEMENTO_SIZE_CLASS_COUNT; i++) {
        heap->caches[i].limit = 64;
    }
#if defined(__cplusplus) || !(defined(_MSC_VER) && !defined(__clang__))
    memento_atomic_ptr_store_relaxed(&heap->foreign_head, NULL);
#else
    heap->foreign_head = NULL;
#endif
    heap->thread_id = memento_get_thread_id();
    heap->numa_node = memento_query_numa_node();
    heap->initialized = 1;
}

static void memento_heap_register(memento_thread_heap_t* heap) {
    MEMENTO_REGISTRY_LOCK();
    heap->registry_next = memento_heap_registry;
    memento_heap_registry = heap;
    MEMENTO_REGISTRY_UNLOCK();
}

/* Forward decls */
static void memento_heap_free_local(memento_thread_heap_t* MEMENTO_RESTRICT heap,
                                    void* MEMENTO_RESTRICT ptr, size_t size);
static void memento_heap_release_caches(memento_thread_heap_t* heap);
static void memento_thread_exit_destructor(void* arg);

memento_thread_heap_t* memento_thread_heap_get(void) {
    if (MEMENTO_UNLIKELY(memento_tls_heap == NULL)) {
        /* Cache-line aligned so per-class pads land on true line boundaries */
        memento_thread_heap_t* heap = (memento_thread_heap_t*)MEMENTO_ALIGNED_ALLOC(
            MEMENTO_CACHE_LINE_SIZE, sizeof(memento_thread_heap_t));
        if (!heap) return NULL;
        memento_heap_init(heap);
        memento_heap_register(heap);
#if MEMENTO_PLATFORM_WINDOWS
        if (memento_fls_ready && memento_fls_index != FLS_OUT_OF_INDEXES) {
            FlsSetValue(memento_fls_index, heap);
        }
#else
        if (memento_tls_key_created) {
            pthread_setspecific(memento_tls_key, heap);
        }
#endif
        memento_tls_heap = heap;
    }
    return memento_tls_heap;
}

/* ============================================================================
 * Cache Operations
 * ============================================================================ */

MEMENTO_FORCE_INLINE void* memento_cache_pop(memento_size_class_cache_t* MEMENTO_RESTRICT cache) {
    void* ptr = cache->head;
    if (MEMENTO_LIKELY(ptr != NULL)) {
        void* next = *(void**)ptr;
        cache->head = next;
        cache->count--;
        if (next) {
            MEMENTO_PREFETCH(next);
        }
        return ptr;
    }
    return NULL;
}

MEMENTO_FORCE_INLINE bool memento_cache_push(memento_size_class_cache_t* MEMENTO_RESTRICT cache,
                                               void* MEMENTO_RESTRICT ptr) {
    if (MEMENTO_UNLIKELY(cache->count >= cache->limit)) {
        return false;
    }
    *(void**)ptr = cache->head;
    cache->head = ptr;
    cache->count++;
    return true;
}

/* ============================================================================
 * Foreign Deallocation - Intrusive MPSC Treiber Stack
 * ============================================================================ */

static void memento_foreign_push(memento_thread_heap_t* heap, void* ptr, size_t size) {
    memento_foreign_node_t* node = (memento_foreign_node_t*)ptr;
    node->size = size;
#if defined(_MSC_VER) && !defined(__clang__) && !defined(__cplusplus)
    for (;;) {
        void* head = memento_atomic_ptr_load_relaxed(&heap->foreign_head);
        node->next = (memento_foreign_node_t*)head;
        if (memento_atomic_ptr_cas_release(&heap->foreign_head, head, (void*)node)) {
            break;
        }
    }
#else
    void* head = memento_atomic_ptr_load_relaxed(&heap->foreign_head);
    do {
        node->next = (memento_foreign_node_t*)head;
    } while (!memento_atomic_ptr_cas_release(&heap->foreign_head, head, (void*)node));
#endif
}

/* Drain foreign frees in batches, sorted by size class for cache locality. */
void memento_thread_heap_flush(memento_thread_heap_t* heap) {
    if (MEMENTO_UNLIKELY(heap == NULL)) return;
    if (MEMENTO_LIKELY(memento_atomic_ptr_load_relaxed(&heap->foreign_head) == NULL)) {
        return;
    }

    memento_foreign_node_t* node =
        (memento_foreign_node_t*)memento_atomic_ptr_exchange_acq(&heap->foreign_head, NULL);

    typedef struct {
        void* ptr;
        size_t size;
        uint8_t sc; /* 0xFF = large */
    } memento_flush_item_t;

    while (node) {
        memento_flush_item_t batch[MEMENTO_FLUSH_BATCH];
        int n = 0;

        while (node && n < MEMENTO_FLUSH_BATCH) {
            memento_foreign_node_t* next = node->next;
            size_t size = node->size;
            batch[n].ptr = node;
            batch[n].size = size;
            batch[n].sc = (size <= MEMENTO_MAX_SIZE_CLASS)
                ? (uint8_t)memento_size_class_for(size)
                : (uint8_t)0xFF;
            node = next;
            n++;
        }

        /* Insertion sort by size class — n <= 64, noise, groups same class */
        for (int i = 1; i < n; i++) {
            memento_flush_item_t key = batch[i];
            int j = i - 1;
            while (j >= 0 && batch[j].sc > key.sc) {
                batch[j + 1] = batch[j];
                j--;
            }
            batch[j + 1] = key;
        }

        for (int i = 0; i < n; i++) {
            memento_heap_free_local(heap, batch[i].ptr, batch[i].size);
            heap->stats.foreign_free_count++;
        }
    }
}

/* ============================================================================
 * OS mapping helpers (mmap / VirtualAlloc) + THP / first-touch
 * ============================================================================ */

static void memento_os_advise_thp(void* ptr, size_t mapped) {
#if MEMENTO_PLATFORM_POSIX && MEMENTO_ENABLE_THP
    #ifdef MADV_HUGEPAGE
    (void)madvise(ptr, mapped, MADV_HUGEPAGE);
    #endif
    #ifdef MADV_WILLNEED
    (void)madvise(ptr, mapped, MADV_WILLNEED);
    #endif
#else
    (void)ptr;
    (void)mapped;
#endif
}

/* First-touch pages so they land on the current thread's NUMA node.
 * Touches one byte per 4 KiB; cheap relative to mmap for multi-page spans. */
static void memento_os_first_touch(void* ptr, size_t mapped) {
#if MEMENTO_ENABLE_NUMA
    volatile char* p = (volatile char*)ptr;
    for (size_t off = 0; off < mapped; off += 4096u) {
        p[off] = 0;
    }
#else
    (void)ptr;
    (void)mapped;
#endif
}

static void* memento_os_alloc(size_t size) {
    size_t mapped = memento_align_up(size, 4096);
    if (MEMENTO_UNLIKELY(mapped == 0)) mapped = 4096;
    void* ptr = MEMENTO_MMAP(mapped);
#if MEMENTO_PLATFORM_POSIX
    if (MEMENTO_UNLIKELY(ptr == MAP_FAILED)) return NULL;
#endif
    if (MEMENTO_UNLIKELY(ptr == NULL)) return NULL;
    memento_os_advise_thp(ptr, mapped);
    memento_os_first_touch(ptr, mapped);
    return ptr;
}

static void memento_os_free(void* ptr, size_t size) {
    if (MEMENTO_UNLIKELY(!ptr)) return;
    size_t mapped = memento_align_up(size, 4096);
    if (MEMENTO_UNLIKELY(mapped == 0)) mapped = 4096;
    MEMENTO_MUNMAP(ptr, mapped);
}

/* ============================================================================
 * 2 MiB span bump allocator (size-class refill)
 * ============================================================================ */

static memento_span_t* memento_span_create(void) {
    void* mem = memento_os_alloc(MEMENTO_SPAN_SIZE);
    if (MEMENTO_UNLIKELY(!mem)) return NULL;
    memento_span_t* span = (memento_span_t*)mem;
    span->next = NULL;
    span->capacity = MEMENTO_SPAN_SIZE - MEMENTO_SPAN_HEADER;
    /* Align first block to 16 bytes (header may leave data misaligned) */
    {
        uintptr_t data = (uintptr_t)span->data;
        uintptr_t aligned = (data + 15u) & ~(uintptr_t)15u;
        span->used = (size_t)(aligned - data);
    }
    return span;
}

/* Bump-allocate `size` bytes (aligned to 16) from the current span, mapping
 * a new span if needed. Returns NULL only on OS allocation failure. */
static void* memento_span_bump(memento_thread_heap_t* MEMENTO_RESTRICT heap, size_t size) {
    size = memento_align_up(size, 16);
    if (MEMENTO_UNLIKELY(size > MEMENTO_SPAN_SIZE - MEMENTO_SPAN_HEADER)) {
        /* Single block larger than a span's payload — direct OS map */
        return memento_os_alloc(size);
    }

    memento_span_t* span = heap->span_current;
    if (MEMENTO_UNLIKELY(!span || span->used + size > span->capacity)) {
        span = memento_span_create();
        if (!span) return NULL;
        span->next = heap->span_list;
        heap->span_list = span;
        heap->span_current = span;
    }

    void* ptr = span->data + span->used;
    span->used += size;
    return ptr;
}

/* Carve a batch of size-class blocks into the freelist; return one for use. */
static void* memento_refill_size_class(memento_thread_heap_t* MEMENTO_RESTRICT heap,
                                        size_t sc) {
    size_t block_size = memento_size_class_to_size(sc);
    memento_size_class_cache_t* MEMENTO_RESTRICT cache = &heap->caches[sc];

    uint32_t room = (cache->count < cache->limit)
        ? (cache->limit - cache->count)
        : 0;
    uint32_t batch = MEMENTO_REFILL_BATCH;
    if (batch > room + 1) {
        batch = room + 1; /* +1 for the block we return */
    }
    if (batch < 1) batch = 1;

    /* Prefer carving from remaining span space in one contiguous run */
    memento_span_t* span = heap->span_current;
    uint32_t fit = batch;
    if (span) {
        size_t remain = (span->used < span->capacity) ? (span->capacity - span->used) : 0;
        uint32_t max_fit = (uint32_t)(remain / block_size);
        if (max_fit == 0) {
            /* force new span on next bump */
        } else if (max_fit < fit) {
            fit = max_fit;
        }
    }

    void* first = NULL;
    for (uint32_t i = 0; i < fit; i++) {
        void* block = memento_span_bump(heap, block_size);
        if (MEMENTO_UNLIKELY(!block)) {
            break;
        }
        heap->stats.bytes_allocated += block_size;
        if (!first) {
            first = block;
            memento_debug_mark_fresh(block);
        } else {
            /* Park extras on the freelist (never allocated → no double-free check) */
            memento_debug_mark_free(block);
            *(void**)block = cache->head;
            cache->head = block;
            cache->count++;
            MEMENTO_ASAN_POISON(block, block_size);
        }
    }
    return first;
}

/* ============================================================================
 * Large-object cache (exact user size)
 * ============================================================================ */

MEMENTO_FORCE_INLINE void* memento_large_cache_take(memento_thread_heap_t* MEMENTO_RESTRICT heap,
                                                      size_t size) {
    uint32_t n = heap->large_count;
    for (uint32_t i = 0; i < n; i++) {
        if (heap->large_cache[i].size == size) {
            void* ptr = heap->large_cache[i].ptr;
            /* swap-remove */
            heap->large_cache[i] = heap->large_cache[n - 1];
            heap->large_count = n - 1;
            return ptr;
        }
    }
    return NULL;
}

MEMENTO_FORCE_INLINE bool memento_large_cache_put(memento_thread_heap_t* MEMENTO_RESTRICT heap,
                                                    void* ptr, size_t size) {
    if (size > MEMENTO_LARGE_CACHE_MAX_SIZE) return false;
    if (heap->large_count >= MEMENTO_LARGE_CACHE_SLOTS) return false;
    heap->large_cache[heap->large_count].ptr = ptr;
    heap->large_cache[heap->large_count].size = size;
    heap->large_count++;
    return true;
}

/* ============================================================================
 * Thread Heap Allocation — fast path (cache hit) + slow path (span / large)
 * ============================================================================ */

/* Slow path: cache miss or large allocation. Kept out-of-line so the hot
 * path stays tiny and I-cache friendly. */
static MEMENTO_NOINLINE void* memento_slow_alloc(memento_thread_heap_t* MEMENTO_RESTRICT heap,
                                                   size_t size, size_t sc) {
    if (MEMENTO_LIKELY(size <= MEMENTO_MAX_SIZE_CLASS)) {
        void* ptr = memento_refill_size_class(heap, sc);
        if (ptr) {
            heap->stats.alloc_count++;
        }
        return ptr;
    }

    /* Large allocation: try exact-size cache first */
    void* cached = memento_large_cache_take(heap, size);
    if (cached) {
        heap->stats.alloc_count++;
        return cached;
    }

    /* Size header before user pointer (for foreign free / munmap sizing) */
    if (size > SIZE_MAX - sizeof(size_t)) {
        return NULL;
    }
    size_t total_size = size + sizeof(size_t);
    void* raw = memento_os_alloc(total_size);
    if (raw) {
        *(size_t*)raw = size;
        heap->stats.alloc_count++;
        heap->stats.bytes_allocated += size;
        return (char*)raw + sizeof(size_t);
    }
    return NULL;
}

/* Hot path: single cache-line touch on freelist hit. */
MEMENTO_FORCE_INLINE void* memento_fast_alloc(memento_thread_heap_t* MEMENTO_RESTRICT heap,
                                               size_t size) {
    if (MEMENTO_UNLIKELY(heap == NULL || size == 0)) {
        return NULL;
    }

    if (MEMENTO_LIKELY(size <= MEMENTO_MAX_SIZE_CLASS)) {
        size_t sc = (size_t)memento_sc_lut[(size + 15) >> 4];
        memento_size_class_cache_t* MEMENTO_RESTRICT cache = &heap->caches[sc];
        void* MEMENTO_RESTRICT ptr = cache->head;
        if (MEMENTO_LIKELY(ptr != NULL)) {
            size_t block_size = memento_size_class_to_size(sc);
            MEMENTO_ASAN_UNPOISON(ptr, block_size);
            void* next = *(void**)ptr;
            cache->head = next;
            cache->count--;
            if (next) {
                MEMENTO_PREFETCH(next);
            }
            memento_debug_on_alloc(ptr);
            heap->stats.alloc_count++;
            return ptr;
        }
        return memento_slow_alloc(heap, size, sc);
    }

    return memento_slow_alloc(heap, size, 0);
}

void* memento_thread_heap_alloc(memento_thread_heap_t* heap, size_t size) {
    return memento_fast_alloc(heap, size);
}

/* Branchless freelist push for size-class blocks (LUT mirrored from alloc). */
MEMENTO_FORCE_INLINE void memento_free_size_class(memento_thread_heap_t* MEMENTO_RESTRICT heap,
                                                    void* MEMENTO_RESTRICT ptr, size_t size) {
    size_t sc = (size_t)memento_sc_lut[(size + 15) >> 4];
    size_t block_size = memento_size_class_to_size(sc);
    memento_size_class_cache_t* MEMENTO_RESTRICT cache = &heap->caches[sc];

    memento_debug_on_free(ptr);
    *(void**)ptr = cache->head;
    cache->head = ptr;
    cache->count++;
    /* Poison user payload while on freelist (next ptr stays readable under ASan
     * if we only poison past the header — poison whole block after link write). */
    MEMENTO_ASAN_POISON(ptr, block_size);
    heap->stats.free_count++;
}

static void memento_heap_free_local(memento_thread_heap_t* MEMENTO_RESTRICT heap,
                                     void* MEMENTO_RESTRICT ptr, size_t size) {
    if (MEMENTO_LIKELY(size <= MEMENTO_MAX_SIZE_CLASS)) {
        /* Span-backed: always park on freelist (limit is refill-only). */
        memento_free_size_class(heap, ptr, size);
        return;
    }

    /* Large: try LOC before munmap */
    if (memento_large_cache_put(heap, ptr, size)) {
        heap->stats.free_count++;
        return;
    }
    memento_os_free((char*)ptr - sizeof(size_t), size + sizeof(size_t));
    heap->stats.free_count++;
    heap->stats.bytes_freed += size;
}

void memento_thread_heap_free(memento_thread_heap_t* heap, void* ptr, size_t size) {
    if (MEMENTO_UNLIKELY(ptr == NULL || heap == NULL)) {
        return;
    }

    if (MEMENTO_UNLIKELY(heap != memento_tls_heap)) {
        memento_foreign_push(heap, ptr, size);
        return;
    }

    memento_heap_free_local(heap, ptr, size);
}

void* memento_thread_heap_realloc(memento_thread_heap_t* heap, void* ptr,
                                   size_t old_size, size_t new_size) {
    if (ptr == NULL) {
        return memento_thread_heap_alloc(heap, new_size);
    }
    if (new_size == 0) {
        memento_thread_heap_free(heap, ptr, old_size);
        return NULL;
    }

    /* Same size class: usable capacity already covers new_size — no copy */
    if (old_size <= MEMENTO_MAX_SIZE_CLASS && new_size <= MEMENTO_MAX_SIZE_CLASS) {
        size_t old_sc = memento_size_class_for(old_size);
        size_t new_sc = memento_size_class_for(new_size);
        if (old_sc == new_sc) {
            return ptr;
        }
    } else if (old_size == new_size) {
        return ptr;
    }

    void* new_ptr = memento_thread_heap_alloc(heap, new_size);
    if (new_ptr) {
        size_t copy_size = (old_size < new_size) ? old_size : new_size;
        /* Only copy the caller's logical bytes, not padded class size */
        memcpy(new_ptr, ptr, copy_size);
        memento_thread_heap_free(heap, ptr, old_size);
    }
    return new_ptr;
}

void memento_thread_heap_stats(memento_thread_heap_t* heap, memento_heap_stats_t* stats) {
    if (heap && stats) {
        *stats = heap->stats;
    }
}

int memento_thread_heap_numa_node(const memento_thread_heap_t* heap) {
    if (MEMENTO_UNLIKELY(!heap)) return -1;
    return (int)heap->numa_node;
}

/* ============================================================================
 * Global Initialization
 * ============================================================================ */

static void memento_heap_release_caches(memento_thread_heap_t* heap) {
    /* Size-class freelist nodes live inside spans — drop the lists, then unmap. */
    for (int i = 0; i < MEMENTO_SIZE_CLASS_COUNT; i++) {
        heap->caches[i].head = NULL;
        heap->caches[i].count = 0;
    }

    /* Large-object cache → OS */
    for (uint32_t i = 0; i < heap->large_count; i++) {
        void* ptr = heap->large_cache[i].ptr;
        size_t size = heap->large_cache[i].size;
        if (ptr) {
            memento_os_free((char*)ptr - sizeof(size_t), size + sizeof(size_t));
        }
    }
    heap->large_count = 0;

    /* Unmap every span */
    memento_span_t* span = heap->span_list;
    heap->span_list = NULL;
    heap->span_current = NULL;
    while (span) {
        memento_span_t* next = span->next;
        memento_os_free(span, MEMENTO_SPAN_SIZE);
        span = next;
    }
}

/* pthread/FLS TLS destructor: runs on thread exit (not for the main thread).
 * Drain foreign frees that arrived while we were still "owner" and mark the
 * heap retired. Do NOT release spans/caches here: live allocations may still
 * be held by other threads and foreign-freed later (they write into the block
 * itself for the MPSC node). Spans stay mapped until memento_shutdown. */
static void memento_thread_exit_destructor(void* arg) {
    memento_thread_heap_t* heap = (memento_thread_heap_t*)arg;
    if (!heap) return;
    memento_thread_heap_flush(heap);
    heap->retired = 1;
    memento_tls_heap = NULL;
}

bool memento_init(void) {
    if (memento_initialized) {
        return true;
    }
#if MEMENTO_PLATFORM_WINDOWS
    memento_registry_lock_ensure();
    if (!memento_fls_ready) {
        memento_fls_index = FlsAlloc(memento_thread_exit_destructor);
        if (memento_fls_index == FLS_OUT_OF_INDEXES) {
            return false;
        }
        memento_fls_ready = 1;
    }
#else
    if (!memento_tls_key_created) {
        if (pthread_key_create(&memento_tls_key, memento_thread_exit_destructor) == 0) {
            memento_tls_key_created = 1;
        }
    }
#endif
    memento_initialized = 1;
    return true;
}

void memento_shutdown(void) {
    if (!memento_initialized) return;

    MEMENTO_REGISTRY_LOCK();
    memento_thread_heap_t* heap = memento_heap_registry;
    memento_heap_registry = NULL;
    MEMENTO_REGISTRY_UNLOCK();

    while (heap) {
        memento_thread_heap_t* next = heap->registry_next;
        memento_foreign_node_t* node =
            (memento_foreign_node_t*)memento_atomic_ptr_exchange_acq(&heap->foreign_head, NULL);
        while (node) {
            memento_foreign_node_t* n2 = node->next;
            memento_heap_free_local(heap, node, node->size);
            node = n2;
        }
        memento_heap_release_caches(heap);
        if (heap == memento_tls_heap) {
            memento_tls_heap = NULL;
#if MEMENTO_PLATFORM_WINDOWS
            if (memento_fls_ready && memento_fls_index != FLS_OUT_OF_INDEXES) {
                FlsSetValue(memento_fls_index, NULL);
            }
#else
            if (memento_tls_key_created) {
                pthread_setspecific(memento_tls_key, NULL);
            }
#endif
        }
        MEMENTO_ALIGNED_FREE(heap);
        heap = next;
    }

#if MEMENTO_PLATFORM_WINDOWS
    if (memento_fls_ready && memento_fls_index != FLS_OUT_OF_INDEXES) {
        FlsFree(memento_fls_index);
        memento_fls_index = FLS_OUT_OF_INDEXES;
        memento_fls_ready = 0;
    }
    if (memento_registry_lock_init) {
        DeleteCriticalSection(&memento_registry_lock);
        memento_registry_lock_init = 0;
    }
#else
    if (memento_tls_key_created) {
        pthread_key_delete(memento_tls_key);
        memento_tls_key_created = 0;
    }
#endif

    memento_initialized = 0;
}

/* ============================================================================
 * Pool Allocator
 * ============================================================================ */

typedef struct memento_pool_chunk_s {
    struct memento_pool_chunk_s* next;
} memento_pool_chunk_t;

struct memento_pool_s {
    memento_pool_chunk_t* free_list;
    size_t object_size;
    size_t capacity;
    size_t count;
    memento_thread_heap_t* heap;
    void* blocks;
};

memento_pool_t* memento_pool_create(size_t object_size, size_t capacity,
                                     memento_thread_heap_t* heap) {
    if (capacity == 0) return NULL;
    if (object_size < sizeof(void*)) {
        object_size = sizeof(void*);
    }

    size_t block_size;
    if (memento_mul_overflow(object_size, capacity, &block_size)) {
        return NULL;
    }

    memento_pool_t* pool = (memento_pool_t*)MEMENTO_MALLOC(sizeof(memento_pool_t));
    if (!pool) return NULL;

    pool->object_size = object_size;
    pool->capacity = capacity;
    pool->count = capacity;
    pool->heap = heap ? heap : memento_thread_heap_get();
    pool->free_list = NULL;
    pool->blocks = NULL;

    if (!pool->heap) {
        MEMENTO_FREE(pool, sizeof(memento_pool_t));
        return NULL;
    }

    void* block = memento_thread_heap_alloc(pool->heap, block_size);
    if (!block) {
        MEMENTO_FREE(pool, sizeof(memento_pool_t));
        return NULL;
    }

    for (size_t i = 0; i < capacity; i++) {
        memento_pool_chunk_t* chunk =
            (memento_pool_chunk_t*)((char*)block + i * object_size);
        chunk->next = pool->free_list;
        pool->free_list = chunk;
    }
    pool->blocks = block;
    return pool;
}

void memento_pool_destroy(memento_pool_t* pool) {
    if (!pool) return;
    if (pool->blocks) {
        size_t block_size = pool->object_size * pool->capacity; /* already validated */
        memento_thread_heap_free(pool->heap, pool->blocks, block_size);
    }
    MEMENTO_FREE(pool, sizeof(memento_pool_t));
}

void* memento_pool_alloc(memento_pool_t* pool) {
    if (!pool || !pool->free_list) return NULL;
    memento_pool_chunk_t* chunk = pool->free_list;
    pool->free_list = chunk->next;
    pool->count--;
    return chunk;
}

void memento_pool_free(memento_pool_t* pool, void* ptr) {
    if (!pool || !ptr) return;
    memento_pool_chunk_t* chunk = (memento_pool_chunk_t*)ptr;
    chunk->next = pool->free_list;
    pool->free_list = chunk;
    pool->count++;
}

/* ============================================================================
 * Arena Allocator
 * ============================================================================ */

typedef struct memento_arena_block_s {
    struct memento_arena_block_s* next;
    size_t size;
    char data[1]; /* flexible; actual capacity is in size */
} memento_arena_block_t;

/* Bytes from start of block struct to data[] */
#define MEMENTO_ARENA_HEADER_SIZE offsetof(memento_arena_block_t, data)

struct memento_arena_s {
    memento_arena_block_t* current;
    memento_arena_block_t* blocks;
    void* top;
    size_t used;
    size_t capacity;
    size_t initial_capacity;
    memento_thread_heap_t* heap;
};

static memento_arena_block_t* memento_arena_new_block(memento_thread_heap_t* heap,
                                                       size_t capacity) {
    size_t block_size = MEMENTO_ARENA_HEADER_SIZE + capacity;
    memento_arena_block_t* block =
        (memento_arena_block_t*)memento_thread_heap_alloc(heap, block_size);
    if (!block) return NULL;
    block->next = NULL;
    block->size = capacity;
    return block;
}

memento_arena_t* memento_arena_create(size_t initial_capacity,
                                       memento_thread_heap_t* heap) {
    if (initial_capacity == 0) initial_capacity = 4096;

    memento_arena_t* arena = (memento_arena_t*)MEMENTO_MALLOC(sizeof(memento_arena_t));
    if (!arena) return NULL;

    arena->heap = heap ? heap : memento_thread_heap_get();
    if (!arena->heap) {
        MEMENTO_FREE(arena, sizeof(memento_arena_t));
        return NULL;
    }
    arena->initial_capacity = initial_capacity;
    arena->capacity = initial_capacity;
    arena->used = 0;

    arena->current = memento_arena_new_block(arena->heap, initial_capacity);
    if (!arena->current) {
        MEMENTO_FREE(arena, sizeof(memento_arena_t));
        return NULL;
    }

    arena->blocks = arena->current;
    arena->top = arena->current->data;
    return arena;
}

void memento_arena_destroy(memento_arena_t* arena) {
    if (!arena) return;
    memento_arena_block_t* block = arena->blocks;
    while (block) {
        memento_arena_block_t* next = block->next;
        memento_thread_heap_free(arena->heap, block,
                                 MEMENTO_ARENA_HEADER_SIZE + block->size);
        block = next;
    }
    MEMENTO_FREE(arena, sizeof(memento_arena_t));
}

void* memento_arena_alloc(memento_arena_t* arena, size_t size, size_t alignment) {
    if (!arena || size == 0) return NULL;

    if (alignment == 0) {
        alignment = 1;
    }
    if (!memento_is_power_of_two(alignment)) {
        return NULL;
    }

    uintptr_t current = (uintptr_t)arena->top;
    uintptr_t aligned = (current + alignment - 1) & ~(uintptr_t)(alignment - 1);
    size_t padding = (size_t)(aligned - current);

    if (arena->used + padding + size > arena->capacity) {
        size_t need = size + alignment + MEMENTO_ARENA_HEADER_SIZE;
        size_t new_capacity = arena->capacity;
        if (new_capacity == 0) new_capacity = 256;
        while (new_capacity < need) {
            if (new_capacity > SIZE_MAX / 2) {
                new_capacity = need;
                break;
            }
            new_capacity *= 2;
        }
        /* Ensure room for aligned size alone */
        if (new_capacity < size + alignment) {
            new_capacity = size + alignment;
        }

        memento_arena_block_t* block = memento_arena_new_block(arena->heap, new_capacity);
        if (!block) return NULL;

        block->next = arena->blocks;
        arena->blocks = block;
        arena->current = block;
        arena->capacity = new_capacity;
        arena->used = 0;
        arena->top = block->data;

        current = (uintptr_t)arena->top;
        aligned = (current + alignment - 1) & ~(uintptr_t)(alignment - 1);
        padding = (size_t)(aligned - current);
    }

    void* ptr = (void*)aligned;
    arena->top = (char*)aligned + size;
    arena->used += padding + size;
    return ptr;
}

memento_arena_save_t memento_arena_save(memento_arena_t* arena) {
    memento_arena_save_t save;
    if (!arena) {
        memset(&save, 0, sizeof(save));
        return save;
    }
    save.saved_block = arena->current;
    save.saved_top = arena->top;
    save.saved_used = arena->used;
    save.saved_capacity = arena->capacity;
    return save;
}

void memento_arena_restore(memento_arena_t* arena, memento_arena_save_t* save) {
    if (!arena || !save || !save->saved_block) return;
    arena->current = (memento_arena_block_t*)save->saved_block;
    arena->top = save->saved_top;
    arena->used = save->saved_used;
    arena->capacity = save->saved_capacity;
    /* Excess blocks allocated after save remain linked until reset/destroy.
     * They are not used for new allocs until reset walks the chain. */
}

void memento_arena_reset(memento_arena_t* arena) {
    if (!arena) return;

    /* Free every block except keep one of initial_capacity */
    memento_arena_block_t* block = arena->blocks;
    memento_arena_block_t* keep = NULL;

    while (block) {
        memento_arena_block_t* next = block->next;
        if (!keep && block->size == arena->initial_capacity) {
            keep = block;
            keep->next = NULL;
        } else {
            memento_thread_heap_free(arena->heap, block,
                                     MEMENTO_ARENA_HEADER_SIZE + block->size);
        }
        block = next;
    }

    if (!keep) {
        keep = memento_arena_new_block(arena->heap, arena->initial_capacity);
        if (!keep) {
            arena->blocks = NULL;
            arena->current = NULL;
            arena->top = NULL;
            arena->used = 0;
            arena->capacity = 0;
            return;
        }
    }

    arena->blocks = keep;
    arena->current = keep;
    arena->capacity = keep->size;
    arena->used = 0;
    arena->top = keep->data;
}

size_t memento_arena_used(const memento_arena_t* arena) {
    return arena ? arena->used : 0;
}

size_t memento_arena_capacity(const memento_arena_t* arena) {
    return arena ? arena->capacity : 0;
}

/* ============================================================================
 * Stack Allocator
 * ============================================================================ */

struct memento_stack_s {
    char* buffer;
    size_t capacity;
    size_t top;
    memento_thread_heap_t* heap;
};

memento_stack_t* memento_stack_create(size_t capacity, memento_thread_heap_t* heap) {
    if (capacity == 0) return NULL;

    memento_stack_t* stack = (memento_stack_t*)MEMENTO_MALLOC(sizeof(memento_stack_t));
    if (!stack) return NULL;

    stack->heap = heap ? heap : memento_thread_heap_get();
    if (!stack->heap) {
        MEMENTO_FREE(stack, sizeof(memento_stack_t));
        return NULL;
    }
    stack->buffer = (char*)memento_thread_heap_alloc(stack->heap, capacity);
    if (!stack->buffer) {
        MEMENTO_FREE(stack, sizeof(memento_stack_t));
        return NULL;
    }

    stack->capacity = capacity;
    stack->top = 0;
    return stack;
}

void memento_stack_destroy(memento_stack_t* stack) {
    if (!stack) return;
    if (stack->buffer) {
        memento_thread_heap_free(stack->heap, stack->buffer, stack->capacity);
    }
    MEMENTO_FREE(stack, sizeof(memento_stack_t));
}

void* memento_stack_push(memento_stack_t* stack, size_t size, size_t alignment) {
    if (!stack || size == 0) return NULL;

    if (alignment == 0) {
        alignment = 1;
    }
    if (!memento_is_power_of_two(alignment)) {
        return NULL;
    }

    /* Align absolute address, not just the offset — buffer base may be
     * only malloc-aligned (~16B on most platforms). */
    uintptr_t base = (uintptr_t)stack->buffer;
    uintptr_t addr = memento_align_up(base + stack->top, alignment);
    size_t end = (size_t)(addr - base) + size;
    if (end > stack->capacity || end < stack->top) {
        return NULL;
    }

    stack->top = end;
    return (void*)addr;
}

void memento_stack_pop(memento_stack_t* stack, void* ptr) {
    if (!stack || !ptr) return;
    uintptr_t base = (uintptr_t)stack->buffer;
    uintptr_t p = (uintptr_t)ptr;
    if (p < base || p - base > stack->top) return;
    stack->top = (size_t)(p - base);
}

memento_stack_marker_t memento_stack_marker(memento_stack_t* stack) {
    return stack ? stack->top : 0;
}

void memento_stack_pop_to_marker(memento_stack_t* stack, memento_stack_marker_t marker) {
    if (stack && marker <= stack->capacity) {
        stack->top = marker;
    }
}

void memento_stack_reset(memento_stack_t* stack) {
    if (stack) {
        stack->top = 0;
    }
}

/* ============================================================================
 * Slab Allocator
 * ============================================================================ */

typedef struct {
    memento_size_class_cache_t cache;
    size_t block_size;
} memento_slab_class_t;

struct memento_slab_s {
    memento_slab_class_t classes[MEMENTO_SIZE_CLASS_COUNT];
    memento_thread_heap_t* heap;
};

memento_slab_t* memento_slab_create(memento_thread_heap_t* heap) {
    memento_slab_t* slab = (memento_slab_t*)MEMENTO_MALLOC(sizeof(memento_slab_t));
    if (!slab) return NULL;

    slab->heap = heap ? heap : memento_thread_heap_get();
    if (!slab->heap) {
        MEMENTO_FREE(slab, sizeof(memento_slab_t));
        return NULL;
    }

    for (int i = 0; i < MEMENTO_SIZE_CLASS_COUNT; i++) {
        slab->classes[i].cache.head = NULL;
        slab->classes[i].cache.count = 0;
        slab->classes[i].cache.limit = 64;
        slab->classes[i].block_size = memento_size_class_to_size((size_t)i);
    }
    return slab;
}

void memento_slab_destroy(memento_slab_t* slab) {
    if (!slab) return;
    for (int i = 0; i < MEMENTO_SIZE_CLASS_COUNT; i++) {
        void* ptr;
        while ((ptr = memento_cache_pop(&slab->classes[i].cache)) != NULL) {
            memento_thread_heap_free(slab->heap, ptr, slab->classes[i].block_size);
        }
    }
    MEMENTO_FREE(slab, sizeof(memento_slab_t));
}

void* memento_slab_alloc(memento_slab_t* slab, size_t size) {
    if (!slab || size == 0) return NULL;

    if (size > MEMENTO_MAX_SIZE_CLASS) {
        return memento_thread_heap_alloc(slab->heap, size);
    }

    size_t sc = memento_size_class_for(size);
    void* ptr = memento_cache_pop(&slab->classes[sc].cache);
    if (!ptr) {
        ptr = memento_thread_heap_alloc(slab->heap, slab->classes[sc].block_size);
    }
    return ptr;
}

void memento_slab_free(memento_slab_t* slab, void* ptr, size_t size) {
    if (!slab || !ptr) return;

    if (size > MEMENTO_MAX_SIZE_CLASS) {
        memento_thread_heap_free(slab->heap, ptr, size);
        return;
    }

    size_t sc = memento_size_class_for(size);
    if (!memento_cache_push(&slab->classes[sc].cache, ptr)) {
        memento_thread_heap_free(slab->heap, ptr, slab->classes[sc].block_size);
    }
}

#ifdef __cplusplus
  #if defined(__clang__)
    #pragma clang diagnostic pop
  #elif defined(__GNUC__)
    #pragma GCC diagnostic pop
  #endif
#endif

#endif /* MEMENTO_IMPLEMENTATION */

#endif /* MEMENTO_H */
