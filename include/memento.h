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
 * Threading contract:
 * - memento_thread_heap_alloc / realloc / flush: owning thread only
 * - memento_thread_heap_free: ANY thread (exact size required)
 * - When a thread exits, its heap is flushed and retired (pthread TLS
 *   destructor); frees that target a retired heap park on its foreign stack
 *   and are reclaimed by memento_shutdown.
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
#define MEMENTO_VERSION_MINOR 0
#define MEMENTO_VERSION_PATCH 0
#define MEMENTO_VERSION_STRING "2.2.0"
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
#include <assert.h>

/* Atomics for the per-heap foreign-free stack (must be included outside the
 * extern "C" block below) */
#ifdef __cplusplus
    #include <atomic>
#else
    #include <stdatomic.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration and Platform Detection
 * ============================================================================ */

/* Platform detection */
#if defined(_WIN32) || defined(__WIN32__) || defined(_WIN64)
    #define MEMENTO_PLATFORM_WINDOWS 1
    #define MEMENTO_PLATFORM_POSIX 0
    #if defined(_MSC_VER)
        #define MEMENTO_TLS __declspec(thread)
        #define MEMENTO_FORCE_INLINE static __forceinline
        #define MEMENTO_NOINLINE __declspec(noinline)
    #else
        #define MEMENTO_TLS __thread
        #define MEMENTO_FORCE_INLINE static inline __attribute__((always_inline))
        #define MEMENTO_NOINLINE __attribute__((noinline))
    #endif
#else
    #define MEMENTO_PLATFORM_WINDOWS 0
    #define MEMENTO_PLATFORM_POSIX 1
    #define MEMENTO_TLS __thread
    #define MEMENTO_FORCE_INLINE static inline __attribute__((always_inline))
    #define MEMENTO_NOINLINE __attribute__((noinline))
#endif

/* Cache line size (common values: 64 for x86/ARM, 128 for POWER) */
#ifndef MEMENTO_CACHE_LINE_SIZE
    #define MEMENTO_CACHE_LINE_SIZE 64
#endif

/* Alignment macro */
#if defined(_MSC_VER)
    #define MEMENTO_ALIGNED(x) __declspec(align(x))
#else
    #define MEMENTO_ALIGNED(x) __attribute__((aligned(x)))
#endif

/* Atomic pointer - C11 _Atomic in C, std::atomic in C++. Only used for the
 * per-heap foreign-free MPSC stack head; every other heap field is strictly
 * owner-thread-local. */
#ifdef __cplusplus
    typedef std::atomic<void*> memento_atomic_ptr_t;
    #define memento_atomic_ptr_load_relaxed(p)   ((p)->load(std::memory_order_relaxed))
    #define memento_atomic_ptr_exchange_acq(p, v) ((p)->exchange((v), std::memory_order_acquire))
    #define memento_atomic_ptr_cas_release(p, expected, desired) \
        ((p)->compare_exchange_weak((expected), (desired), \
                                    std::memory_order_release, std::memory_order_relaxed))
#else
    typedef _Atomic(void*) memento_atomic_ptr_t;
    #define memento_atomic_ptr_load_relaxed(p)   atomic_load_explicit((p), memory_order_relaxed)
    #define memento_atomic_ptr_exchange_acq(p, v) atomic_exchange_explicit((p), (v), memory_order_acquire)
    #define memento_atomic_ptr_cas_release(p, expected, desired) \
        atomic_compare_exchange_weak_explicit((p), &(expected), (desired), \
                                              memory_order_release, memory_order_relaxed)
#endif

/* ============================================================================
 * Public API
 * ============================================================================ */

typedef struct memento_thread_heap_s memento_thread_heap_t;
typedef struct memento_pool_s memento_pool_t;
typedef struct memento_arena_s memento_arena_t;
typedef struct memento_stack_s memento_stack_t;
typedef struct memento_slab_s memento_slab_t;

/* Size classes: 32B, 64B, 96B, 128B, 192B, 256B, 384B, 512B, 768B, 1KB, 1.5KB, 2KB, 3KB, 4KB, 6KB, 8KB */
#define MEMENTO_SIZE_CLASS_COUNT 16

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

/* Get version string (e.g., "2.2.0") */
static inline const char* memento_version_string(void) {
    return MEMENTO_VERSION_STRING;
}

/* Get version number (e.g., 0x02.2.0 for 2.2.0) */
static inline unsigned int memento_version_number(void) {
    return MEMENTO_VERSION;
}

/* Check if library version is at least major.minor.patch */
static inline int memento_version_check(int major, int minor, int patch) {
    return MEMENTO_VERSION >= ((major << 16) | (minor << 8) | patch);
}

/* ============================================================================
 * Thread Heap API - Non-locking, thread-local caching
 * ============================================================================ */

/* Initialize global state (call once at startup) */
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

/* Get heap statistics */
void memento_thread_heap_stats(memento_thread_heap_t* heap, memento_heap_stats_t* stats);

/* ============================================================================
 * Pool Allocator API - Fixed-size object pools
 * ============================================================================ */

/* Create pool for objects of given size */
memento_pool_t* memento_pool_create(size_t object_size, size_t capacity, 
                                     memento_thread_heap_t* heap);
void memento_pool_destroy(memento_pool_t* pool);

/* Allocate/free from pool */
void* memento_pool_alloc(memento_pool_t* pool);
void memento_pool_free(memento_pool_t* pool, void* ptr);

/* ============================================================================
 * Arena Allocator API - Bump allocator with power-of-2 growth
 * ============================================================================ */

/* Create arena with initial capacity */
memento_arena_t* memento_arena_create(size_t initial_capacity,
                                       memento_thread_heap_t* heap);
void memento_arena_destroy(memento_arena_t* arena);

/* Bump allocation */
void* memento_arena_alloc(memento_arena_t* arena, size_t size, size_t alignment);

/* Save/restore for temporary allocations */
typedef struct {
    void* saved_top;
    size_t saved_used;
} memento_arena_save_t;

memento_arena_save_t memento_arena_save(memento_arena_t* arena);
void memento_arena_restore(memento_arena_t* arena, memento_arena_save_t* save);

/* Reset arena to empty */
void memento_arena_reset(memento_arena_t* arena);

/* Get stats */
size_t memento_arena_used(const memento_arena_t* arena);
size_t memento_arena_capacity(const memento_arena_t* arena);

/* ============================================================================
 * Stack Allocator API - LIFO scope-based allocation
 * ============================================================================ */

/* Create stack with given capacity */
memento_stack_t* memento_stack_create(size_t capacity, memento_thread_heap_t* heap);
void memento_stack_destroy(memento_stack_t* stack);

/* Push/pop allocations (LIFO - must free in reverse order) */
void* memento_stack_push(memento_stack_t* stack, size_t size, size_t alignment);
void memento_stack_pop(memento_stack_t* stack, void* ptr);

/* Frame markers for bulk rollback */
typedef size_t memento_stack_marker_t;

memento_stack_marker_t memento_stack_marker(memento_stack_t* stack);
void memento_stack_pop_to_marker(memento_stack_t* stack, memento_stack_marker_t marker);

/* Reset entire stack */
void memento_stack_reset(memento_stack_t* stack);

/* ============================================================================
 * Slab Allocator API - Multi-size object caching
 * ============================================================================ */

/* Create slab allocator */
memento_slab_t* memento_slab_create(memento_thread_heap_t* heap);
void memento_slab_destroy(memento_slab_t* slab);

/* Allocate/free any size (automatically routed to appropriate size class) */
void* memento_slab_alloc(memento_slab_t* slab, size_t size);
void memento_slab_free(memento_slab_t* slab, void* ptr, size_t size);

/* ============================================================================
 * Utility API
 * ============================================================================ */

/* Size class and alignment utilities are static inline functions defined in
 * the MEMENTO_IMPLEMENTATION block (internal helpers, not public API). */

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

/* The implementation is C11 code. When compiled as C++ (single-TU
 * convenience), silence pedantic warnings about C idioms such as designated
 * initializers, {0} aggregate init, and flexible array members. */
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
 * Internal Implementation - Non-locking Design
 * ============================================================================ */

/* Internal headers */
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>

/* MAP_ANONYMOUS compatibility */
#ifndef MAP_ANONYMOUS
    #ifdef MAP_ANON
        #define MAP_ANONYMOUS MAP_ANON
    #else
        #define MAP_ANONYMOUS 0x20  /* Common value */
    #endif
#endif

/* Branch prediction hints */
#ifndef MEMENTO_LIKELY
    #define MEMENTO_LIKELY(x) __builtin_expect(!!(x), 1)
#endif
#ifndef MEMENTO_UNLIKELY
    #define MEMENTO_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

/* Fallback macros for allocation */
#ifndef MEMENTO_MALLOC
    #define MEMENTO_MALLOC(size) malloc(size)
#endif
#ifndef MEMENTO_FREE
    #define MEMENTO_FREE(ptr, size) free(ptr)
#endif
#ifndef MEMENTO_MMAP
    #define MEMENTO_MMAP(size) mmap(NULL, size, PROT_READ|PROT_WRITE, \
        MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
#endif
#ifndef MEMENTO_MUNMAP
    #define MEMENTO_MUNMAP(ptr, size) munmap(ptr, size)
#endif

/* Size class configuration */
static const size_t memento_size_classes[MEMENTO_SIZE_CLASS_COUNT] = {
    32, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192
};

MEMENTO_FORCE_INLINE size_t memento_size_class_for(size_t size) {
    if (size <= 32) return 0;
    if (size <= 64) return 1;
    if (size <= 96) return 2;
    if (size <= 128) return 3;
    if (size <= 192) return 4;
    if (size <= 256) return 5;
    if (size <= 384) return 6;
    if (size <= 512) return 7;
    if (size <= 768) return 8;
    if (size <= 1024) return 9;
    if (size <= 1536) return 10;
    if (size <= 2048) return 11;
    if (size <= 3072) return 12;
    if (size <= 4096) return 13;
    if (size <= 6144) return 14;
    return 15;
}

MEMENTO_FORCE_INLINE size_t memento_size_class_to_size(size_t sc) {
    return (sc < MEMENTO_SIZE_CLASS_COUNT) ? memento_size_classes[sc] : 8192;
}

MEMENTO_FORCE_INLINE bool memento_is_power_of_two(size_t x) {
    return (x & (x - 1)) == 0;
}

MEMENTO_FORCE_INLINE size_t memento_align_up(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

MEMENTO_FORCE_INLINE size_t memento_align_down(size_t size, size_t alignment) {
    return size & ~(alignment - 1);
}

/* ============================================================================
 * Internal Thread Cache - Non-locking Treiber Stack
 * ============================================================================ */

typedef struct memento_cache_node_s {
    struct memento_cache_node_s* next;
} memento_cache_node_t;

typedef struct {
    memento_cache_node_t* head;
    uint32_t count;
    uint32_t limit;
} memento_size_class_cache_t;

/* Node written into the first bytes of a foreign-freed block. The smallest
 * size class is 32 bytes, so every block can hold it. */
typedef struct memento_foreign_node_s {
    struct memento_foreign_node_s* next;
    size_t size;    /* exact size the caller passed to free */
} memento_foreign_node_t;

/* Thread-local heap structure */
struct memento_thread_heap_s {
    /* Size class caches - purely owner-thread-local, no atomics needed */
    memento_size_class_cache_t caches[MEMENTO_SIZE_CLASS_COUNT];

    /* Foreign deallocation stack (MPSC): any thread CAS-pushes freed blocks,
     * the owner exchange-drains. Own cache line to avoid false sharing. */
    MEMENTO_ALIGNED(MEMENTO_CACHE_LINE_SIZE) memento_atomic_ptr_t foreign_head;
    char foreign_pad[MEMENTO_CACHE_LINE_SIZE - sizeof(memento_atomic_ptr_t)];

    /* Statistics (owner thread only) */
    memento_heap_stats_t stats;

    /* Thread ID for debugging */
    uint64_t thread_id;

    /* Global registry linkage (guarded by the registry mutex) */
    struct memento_thread_heap_s* registry_next;

    /* Heap state */
    uint8_t initialized;
    uint8_t retired;    /* owning thread has exited */
    uint8_t _pad[6];
};

/* ============================================================================
 * Thread-local Storage
 * ============================================================================ */

static MEMENTO_TLS memento_thread_heap_t* memento_tls_heap = NULL;

/* Global heap registry: every heap ever created, so shutdown can reclaim
 * retired heaps (and any foreign frees parked on them). Cold path only. */
static pthread_mutex_t memento_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static memento_thread_heap_t* memento_heap_registry = NULL;
static pthread_key_t memento_tls_key;
static bool memento_tls_key_created = false;

static uint64_t memento_get_thread_id(void) {
#if defined(__linux__)
    return (uint64_t)pthread_self();
#elif defined(_WIN32)
    return (uint64_t)GetCurrentThreadId();
#else
    static uint64_t counter = 0;
    return ++counter;
#endif
}

static void memento_heap_init(memento_thread_heap_t* heap) {
    memset((void*)heap, 0, sizeof(memento_thread_heap_t));

    for (int i = 0; i < MEMENTO_SIZE_CLASS_COUNT; i++) {
        heap->caches[i].limit = 64; /* Max 64 items per size class */
    }

    heap->thread_id = memento_get_thread_id();
    heap->initialized = 1;
}

static void memento_heap_register(memento_thread_heap_t* heap) {
    pthread_mutex_lock(&memento_registry_lock);
    heap->registry_next = memento_heap_registry;
    memento_heap_registry = heap;
    pthread_mutex_unlock(&memento_registry_lock);
}

/* Get or create thread-local heap */
memento_thread_heap_t* memento_thread_heap_get(void) {
    if (MEMENTO_UNLIKELY(memento_tls_heap == NULL)) {
        memento_thread_heap_t* heap =
            (memento_thread_heap_t*)MEMENTO_MALLOC(sizeof(memento_thread_heap_t));
        if (!heap) return NULL;
        memento_heap_init(heap);
        memento_heap_register(heap);
        /* Arrange for the heap to be flushed and retired on thread exit.
         * (No destructor for the main thread: memento_shutdown handles it.) */
        if (memento_tls_key_created) {
            pthread_setspecific(memento_tls_key, heap);
        }
        memento_tls_heap = heap;
    }
    return memento_tls_heap;
}

/* ============================================================================
 * Cache Operations - Non-locking
 * ============================================================================ */

MEMENTO_FORCE_INLINE void* memento_cache_pop(memento_size_class_cache_t* cache) {
    memento_cache_node_t* node = cache->head;
    if (MEMENTO_LIKELY(node != NULL)) {
        cache->head = node->next;
        cache->count--;
        return node;
    }
    return NULL;
}

MEMENTO_FORCE_INLINE bool memento_cache_push(memento_size_class_cache_t* cache, void* ptr) {
    if (cache->count >= cache->limit) {
        return false; /* Cache full */
    }
    memento_cache_node_t* node = (memento_cache_node_t*)ptr;
    node->next = cache->head;
    cache->head = node;
    cache->count++;
    return true;
}

/* ============================================================================
 * Foreign Deallocation - Intrusive MPSC Treiber Stack
 *
 * A foreign free writes {next, size} into the freed block itself and
 * CAS-pushes it (release). The owner exchange-drains the whole stack
 * (acquire) and frees each entry locally with its exact size. Unbounded,
 * zero per-allocation overhead, no head/tail index races.
 * ============================================================================ */

static void memento_foreign_push(memento_thread_heap_t* heap, void* ptr, size_t size) {
    memento_foreign_node_t* node = (memento_foreign_node_t*)ptr;
    node->size = size;
    void* head = memento_atomic_ptr_load_relaxed(&heap->foreign_head);
    do {
        node->next = (memento_foreign_node_t*)head;
    } while (!memento_atomic_ptr_cas_release(&heap->foreign_head, head, (void*)node));
}

/* Forward decl: local free that skips the ownership check (used by flush and
 * by shutdown, which drains retired heaps from a different thread). */
static void memento_heap_free_local(memento_thread_heap_t* heap, void* ptr, size_t size);

void memento_thread_heap_flush(memento_thread_heap_t* heap) {
    if (MEMENTO_UNLIKELY(heap == NULL)) return;
    if (MEMENTO_LIKELY(memento_atomic_ptr_load_relaxed(&heap->foreign_head) == NULL)) return;

    memento_foreign_node_t* node =
        (memento_foreign_node_t*)memento_atomic_ptr_exchange_acq(&heap->foreign_head, NULL);
    while (node) {
        memento_foreign_node_t* next = node->next;
        size_t size = node->size;
        memento_heap_free_local(heap, node, size);
        heap->stats.foreign_free_count++;
        node = next;
    }
}

/* ============================================================================
 * Allocation from System
 * ============================================================================ */

static void* memento_alloc_from_system(size_t size) {
    /* Use mmap for large allocations, malloc for small */
    if (size >= 64 * 1024) {
        size = memento_align_up(size, 4096);
        return MEMENTO_MMAP(size);
    }
    return MEMENTO_MALLOC(size);
}

static void memento_free_to_system(void* ptr, size_t size) {
    if (size >= 64 * 1024) {
        size = memento_align_up(size, 4096);
        MEMENTO_MUNMAP(ptr, size);
    } else {
        MEMENTO_FREE(ptr, size);
    }
}

/* ============================================================================
 * Thread Heap Allocation
 * ============================================================================ */

void* memento_thread_heap_alloc(memento_thread_heap_t* heap, size_t size) {
    if (MEMENTO_UNLIKELY(heap == NULL || size == 0)) {
        return NULL;
    }
    
    /* Check for size classes */
    if (size <= 8192) {
        size_t sc = memento_size_class_for(size);
        size_t actual_size = memento_size_class_to_size(sc);
        
        /* Try thread-local cache first */
        void* ptr = memento_cache_pop(&heap->caches[sc]);
        if (MEMENTO_LIKELY(ptr != NULL)) {
            heap->stats.alloc_count++;
            return ptr;
        }
        
        /* Allocate from system */
        ptr = memento_alloc_from_system(actual_size);
        if (ptr) {
            heap->stats.alloc_count++;
            heap->stats.bytes_allocated += actual_size;
        }
        return ptr;
    }
    
    /* Large allocation - direct from system with size header for foreign free */
    size_t total_size = size + sizeof(size_t);
    void* ptr = memento_alloc_from_system(total_size);
    if (ptr) {
        *(size_t*)ptr = size;
        heap->stats.alloc_count++;
        heap->stats.bytes_allocated += size;
        return (char*)ptr + sizeof(size_t);
    }
    return NULL;
}

/* Local free path: caller guarantees it is safe to touch the heap's caches
 * (owning thread, or shutdown draining a retired heap after all threads
 * were joined). */
static void memento_heap_free_local(memento_thread_heap_t* heap, void* ptr, size_t size) {
    /* Try to cache small blocks */
    if (size <= 8192) {
        size_t sc = memento_size_class_for(size);
        if (memento_cache_push(&heap->caches[sc], ptr)) {
            heap->stats.free_count++;
            return;
        }
        memento_free_to_system(ptr, memento_size_class_to_size(sc));
    } else {
        /* Large allocation: the size header sits before the user pointer */
        memento_free_to_system((char*)ptr - sizeof(size_t), size + sizeof(size_t));
    }
    heap->stats.free_count++;
    heap->stats.bytes_freed += size;
}

void memento_thread_heap_free(memento_thread_heap_t* heap, void* ptr, size_t size) {
    if (MEMENTO_UNLIKELY(ptr == NULL || heap == NULL)) {
        return;
    }

    /* Foreign free (any thread other than the owner): push the block itself
     * onto the heap's MPSC stack; the owner reclaims it in flush. The block
     * is always big enough for the node (min size class 32B; large blocks
     * trivially so). */
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
    
    void* new_ptr = memento_thread_heap_alloc(heap, new_size);
    if (new_ptr) {
        size_t copy_size = (old_size < new_size) ? old_size : new_size;
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

/* ============================================================================
 * Global Initialization
 * ============================================================================ */

static bool memento_initialized = false;

/* Return every cached block to the system (owner thread or shutdown only) */
static void memento_heap_release_caches(memento_thread_heap_t* heap) {
    for (int i = 0; i < MEMENTO_SIZE_CLASS_COUNT; i++) {
        void* ptr;
        while ((ptr = memento_cache_pop(&heap->caches[i])) != NULL) {
            memento_free_to_system(ptr, memento_size_class_to_size((size_t)i));
        }
    }
}

/* pthread TLS destructor: runs on thread exit (not for the main thread).
 * Drain what we can and mark the heap retired; the struct itself must stay
 * alive because other threads may still push foreign frees to it. It is
 * reclaimed by memento_shutdown. */
static void memento_thread_exit_destructor(void* arg) {
    memento_thread_heap_t* heap = (memento_thread_heap_t*)arg;
    if (!heap) return;
    memento_thread_heap_flush(heap);
    memento_heap_release_caches(heap);
    heap->retired = 1;
    memento_tls_heap = NULL;
}

bool memento_init(void) {
    if (memento_initialized) {
        return true;
    }
    if (!memento_tls_key_created) {
        if (pthread_key_create(&memento_tls_key, memento_thread_exit_destructor) == 0) {
            memento_tls_key_created = true;
        }
    }
    memento_initialized = true;
    return true;
}

/* Reclaim every heap in the registry. Contract: no other thread may touch
 * memento after this begins (join threads first) - then draining foreign
 * stacks from this thread is race-free. */
void memento_shutdown(void) {
    if (!memento_initialized) return;

    pthread_mutex_lock(&memento_registry_lock);
    memento_thread_heap_t* heap = memento_heap_registry;
    memento_heap_registry = NULL;
    pthread_mutex_unlock(&memento_registry_lock);

    while (heap) {
        memento_thread_heap_t* next = heap->registry_next;
        /* Drain any parked foreign frees, then release caches. Safe from this
         * thread by the shutdown contract. */
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
            if (memento_tls_key_created) pthread_setspecific(memento_tls_key, NULL);
        }
        MEMENTO_FREE(heap, sizeof(memento_thread_heap_t));
        heap = next;
    }

    memento_initialized = false;
}

/* ============================================================================
 * Pool Allocator Implementation
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
    void* blocks;  /* Linked list of allocated blocks for cleanup */
};

memento_pool_t* memento_pool_create(size_t object_size, size_t capacity,
                                     memento_thread_heap_t* heap) {
    if (object_size < sizeof(void*)) {
        object_size = sizeof(void*);
    }
    
    memento_pool_t* pool = (memento_pool_t*)MEMENTO_MALLOC(sizeof(memento_pool_t));
    if (!pool) return NULL;
    
    pool->object_size = object_size;
    pool->capacity = capacity;
    pool->count = capacity;   /* number of objects currently available */
    pool->heap = heap ? heap : memento_thread_heap_get();
    pool->free_list = NULL;
    pool->blocks = NULL;
    
    /* Pre-allocate objects */
    size_t block_size = object_size * capacity;
    void* block = memento_thread_heap_alloc(pool->heap, block_size);
    if (!block) {
        MEMENTO_FREE(pool, sizeof(memento_pool_t));
        return NULL;
    }
    
    /* Build free list */
    for (size_t i = 0; i < capacity; i++) {
        memento_pool_chunk_t* chunk = (memento_pool_chunk_t*)((char*)block + i * object_size);
        chunk->next = pool->free_list;
        pool->free_list = chunk;
    }
    pool->blocks = block;

    return pool;
}

void memento_pool_destroy(memento_pool_t* pool) {
    if (!pool) return;
    if (pool->blocks) {
        memento_thread_heap_free(pool->heap, pool->blocks,
                                 pool->object_size * pool->capacity);
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
 * Arena Allocator Implementation
 * ============================================================================ */

typedef struct memento_arena_block_s {
    struct memento_arena_block_s* next;
    size_t size;
    char data[];
} memento_arena_block_t;

struct memento_arena_s {
    memento_arena_block_t* current;
    memento_arena_block_t* blocks;
    void* top;
    size_t used;
    size_t capacity;
    size_t initial_capacity;
    memento_thread_heap_t* heap;
};

memento_arena_t* memento_arena_create(size_t initial_capacity,
                                       memento_thread_heap_t* heap) {
    memento_arena_t* arena = (memento_arena_t*)MEMENTO_MALLOC(sizeof(memento_arena_t));
    if (!arena) return NULL;
    
    arena->heap = heap ? heap : memento_thread_heap_get();
    arena->initial_capacity = initial_capacity;
    arena->capacity = initial_capacity;
    arena->used = 0;
    
    /* Allocate initial block */
    size_t block_size = sizeof(memento_arena_block_t) + initial_capacity;
    arena->current = (memento_arena_block_t*)memento_thread_heap_alloc(arena->heap, block_size);
    if (!arena->current) {
        MEMENTO_FREE(arena, sizeof(memento_arena_t));
        return NULL;
    }
    
    arena->current->next = NULL;
    arena->current->size = initial_capacity;
    arena->blocks = arena->current;
    arena->top = arena->current->data;
    
    return arena;
}

void memento_arena_destroy(memento_arena_t* arena) {
    if (!arena) return;
    
    memento_arena_block_t* block = arena->blocks;
    while (block) {
        memento_arena_block_t* next = block->next;
        memento_thread_heap_free(arena->heap, block, sizeof(memento_arena_block_t) + block->size);
        block = next;
    }
    
    MEMENTO_FREE(arena, sizeof(memento_arena_t));
}

void* memento_arena_alloc(memento_arena_t* arena, size_t size, size_t alignment) {
    if (!arena || size == 0) return NULL;
    
    /* Align the current position */
    uintptr_t current = (uintptr_t)arena->top;
    uintptr_t aligned = (current + alignment - 1) & ~(alignment - 1);
    size_t padding = aligned - current;
    
    /* Check if we need to grow */
    if (arena->used + padding + size > arena->capacity) {
        /* Grow with power of 2 */
        size_t new_capacity = arena->capacity * 2;
        while (new_capacity < size + sizeof(memento_arena_block_t)) {
            new_capacity *= 2;
        }
        
        size_t block_size = sizeof(memento_arena_block_t) + new_capacity;
        memento_arena_block_t* block = (memento_arena_block_t*)memento_thread_heap_alloc(arena->heap, block_size);
        if (!block) return NULL;
        
        block->next = arena->blocks;
        block->size = new_capacity;
        arena->blocks = block;
        arena->current = block;
        arena->capacity = new_capacity;
        arena->used = 0;
        arena->top = block->data;
        
        /* Recalculate alignment */
        current = (uintptr_t)arena->top;
        aligned = (current + alignment - 1) & ~(alignment - 1);
    }
    
    void* ptr = (void*)aligned;
    arena->top = (char*)aligned + size;
    arena->used += padding + size;
    
    return ptr;
}

memento_arena_save_t memento_arena_save(memento_arena_t* arena) {
    memento_arena_save_t save;
    save.saved_top = arena->top;
    save.saved_used = arena->used;
    return save;
}

void memento_arena_restore(memento_arena_t* arena, memento_arena_save_t* save) {
    if (!arena || !save) return;
    arena->top = save->saved_top;
    arena->used = save->saved_used;
}

void memento_arena_reset(memento_arena_t* arena) {
    if (!arena) return;
    arena->top = arena->current->data;
    arena->used = 0;
}

size_t memento_arena_used(const memento_arena_t* arena) {
    return arena ? arena->used : 0;
}

size_t memento_arena_capacity(const memento_arena_t* arena) {
    return arena ? arena->capacity : 0;
}

/* ============================================================================
 * Stack Allocator Implementation
 * ============================================================================ */

struct memento_stack_s {
    char* buffer;
    size_t capacity;
    size_t top;
    memento_thread_heap_t* heap;
};

memento_stack_t* memento_stack_create(size_t capacity, memento_thread_heap_t* heap) {
    memento_stack_t* stack = (memento_stack_t*)MEMENTO_MALLOC(sizeof(memento_stack_t));
    if (!stack) return NULL;
    
    stack->heap = heap ? heap : memento_thread_heap_get();
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
    if (!stack) return NULL;
    
    size_t aligned_top = memento_align_up(stack->top, alignment);
    if (aligned_top + size > stack->capacity) {
        return NULL; /* Stack overflow */
    }
    
    void* ptr = stack->buffer + aligned_top;
    stack->top = aligned_top + size;
    return ptr;
}

void memento_stack_pop(memento_stack_t* stack, void* ptr) {
    if (!stack || !ptr) return;
    /* In a real implementation, we'd track sizes to validate LIFO order */
    /* For now, this is a no-op - the memory is just reused on next push */
    (void)ptr;
}

memento_stack_marker_t memento_stack_marker(memento_stack_t* stack) {
    return stack ? stack->top : 0;
}

void memento_stack_pop_to_marker(memento_stack_t* stack, memento_stack_marker_t marker) {
    if (stack) {
        stack->top = marker;
    }
}

void memento_stack_reset(memento_stack_t* stack) {
    if (stack) {
        stack->top = 0;
    }
}

/* ============================================================================
 * Slab Allocator Implementation
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
    
    for (int i = 0; i < MEMENTO_SIZE_CLASS_COUNT; i++) {
        slab->classes[i].cache.head = NULL;
        slab->classes[i].cache.count = 0;
        slab->classes[i].cache.limit = 64;
        slab->classes[i].block_size = memento_size_class_to_size(i);
    }
    
    return slab;
}

void memento_slab_destroy(memento_slab_t* slab) {
    if (!slab) return;
    /* Free all cached blocks */
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
    
    if (size > 8192) {
        /* Large allocation - bypass slab */
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
    
    if (size > 8192) {
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

