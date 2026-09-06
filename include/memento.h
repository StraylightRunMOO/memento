/*
 * Memento Memory Allocator Library — v3.0.0
 *
 * An instrumented multi-strategy allocator for runtimes and dirty libc.
 *
 * The exact-size hot path is lock-free and atomic-free: each thread owns its
 * heap, cross-thread frees travel a per-heap MPSC stack that the owner drains
 * on its own schedule. Around that core sit the tools you reach for when the
 * allocation itself is the bug: sized mode (free without a size), instrumented
 * proxies with cookies/owner/site ids, per-heap reports, a malloc interpose
 * shim, Valgrind/ASan hooks, and heap recycling for worker pools.
 *
 * Allocator strategies:
 * - Thread Heap: thread-local size-class caching; cross-thread free is safe
 * - Pool:  fixed-size object pools
 * - Arena: bump allocator with save/restore that actually returns memory
 * - Stack: LIFO scope-based allocator
 * - Slab:  multi-size object caching
 *
 * Platforms: POSIX (Linux/macOS/BSD, glibc and musl) and Windows
 * (MSVC / MinGW / Clang-CL).
 *
 * Threading contract:
 * - memento_thread_heap_alloc / realloc / flush: owning thread only
 * - memento_thread_heap_free: ANY thread (exact size required in
 *   MEMENTO_EXACT_SIZE mode; ignored in MEMENTO_SIZED mode)
 * - When a thread exits, its heap is flushed and parked (see
 *   memento_heap_park / memento_heap_adopt); the next thread on this worker
 *   adopts it instead of paying for fresh mmaps.
 * - memento_shutdown must happen-after every free targeting any heap
 *   (i.e. join your threads first).
 *
 * Usage (C):
 *   #define MEMENTO_IMPLEMENTATION
 *   #include "memento.h"
 *
 *   int main() {
 *       memento_init();
 *       void* p = memento_malloc(1024);   // sized API: no size at free
 *       memento_free(p);
 *       memento_shutdown();
 *       return 0;
 *   }
 *
 * License: MIT
 */

#ifndef MEMENTO_H
#define MEMENTO_H

/* Ask the C library for POSIX.1-2008 + BSD extensions (madvise, posix_madvise,
 * MAP_ANON spellings). Must come before the first system header to take
 * effect; harmless if the user already included system headers. */
#if !defined(_WIN32)
    #ifndef _DEFAULT_SOURCE
        #define _DEFAULT_SOURCE 1
    #endif
    #ifndef _POSIX_C_SOURCE
        #define _POSIX_C_SOURCE 200809L
    #endif
#endif

/* Version macros for compile-time checking */
#define MEMENTO_VERSION_MAJOR 3
#define MEMENTO_VERSION_MINOR 0
#define MEMENTO_VERSION_PATCH 0
#define MEMENTO_VERSION_STRING "3.0.0"
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

/* Atomics for the per-heap foreign-free stack and the park list.
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

/* Compiler hints on the allocating entry points: lets GCC/Clang dead-store-
 * eliminate into freshly allocated memory and fold malloc_usable_size-style
 * queries. Not applied to realloc (its result may alias its argument). */
#if defined(__GNUC__) || defined(__clang__)
    #define MEMENTO_ATTR_MALLOC           __attribute__((malloc))
    #define MEMENTO_ATTR_ALLOC_SIZE(...)  __attribute__((alloc_size(__VA_ARGS__)))
    #define MEMENTO_ATTR_ALLOC_ALIGN(n)   __attribute__((alloc_align(n)))
    #define MEMENTO_ATTR_FLATTEN          __attribute__((flatten))
#else
    #define MEMENTO_ATTR_MALLOC
    #define MEMENTO_ATTR_ALLOC_SIZE(...)
    #define MEMENTO_ATTR_ALLOC_ALIGN(n)
    #define MEMENTO_ATTR_FLATTEN
#endif

/* --- Size contract ---------------------------------------------------------
 * MEMENTO_EXACT_SIZE (default): no per-allocation header on the thread-heap
 * API; the caller supplies the exact size at free. This is the fast path and
 * what suspenders/reaper use.
 *
 * MEMENTO_SIZED: every thread-heap allocation carries a 16-byte header, so
 * memento_thread_heap_free(heap, p, size) can ignore `size` and the global
 * memento_free(p) family works on those pointers too. Costs 16B + one cache
 * line touch per allocation; turn it on for the build where you LD_PRELOAD
 * into a third-party binary. The standalone sized family
 * (memento_malloc/memento_free/memento_heap_malloc/...) is ALWAYS available
 * regardless of this switch — the switch only changes what the thread-heap
 * API itself does. Do not pass exact-mode pointers to memento_free. */
#ifndef MEMENTO_SIZED
    #define MEMENTO_SIZED 0
#endif
#ifndef MEMENTO_EXACT_SIZE
    #define MEMENTO_EXACT_SIZE (!MEMENTO_SIZED)
#endif
#if MEMENTO_SIZED && MEMENTO_EXACT_SIZE
    #error "MEMENTO_SIZED and MEMENTO_EXACT_SIZE cannot both be enabled"
#endif

/* Max size class (bytes); larger goes through the page-run / large path */
#ifndef MEMENTO_MAX_SIZE_CLASS
    #define MEMENTO_MAX_SIZE_CLASS 8192
#endif

/* Thread-local span size for size-class refill (bump-carved blocks).
 * Must be a power of two: spans are aligned to their own size so the owning
 * heap of any small block is one AND away (span->owner). */
#ifndef MEMENTO_SPAN_SIZE
    #define MEMENTO_SPAN_SIZE (2u * 1024u * 1024u)
#endif

/* 64 KiB pages inside each 2 MiB span. Must divide SPAN_SIZE. page_of is
 * one AND; span_of stays the 2 MiB AND. 0 disables the page layer (tests). */
#ifndef MEMENTO_PAGE_SIZE
    #define MEMENTO_PAGE_SIZE 65536u
#endif

/* Per-class LIFO thread cache in front of the active page. slot[0] is a
 * sentinel never popped. Depth is the RSS dial: smaller → less cached.
 * The array is sized to MAX; the live depth is memento_ctl / env. */
#ifndef MEMENTO_TCACHE_DEPTH_MAX
    #define MEMENTO_TCACHE_DEPTH_MAX 64
#endif
#ifndef MEMENTO_TCACHE_DEPTH
    #define MEMENTO_TCACHE_DEPTH 16
#endif
#if MEMENTO_TCACHE_DEPTH > MEMENTO_TCACHE_DEPTH_MAX
    #undef MEMENTO_TCACHE_DEPTH
    #define MEMENTO_TCACHE_DEPTH MEMENTO_TCACHE_DEPTH_MAX
#endif

#ifndef MEMENTO_CPU_HEAPS
    #define MEMENTO_CPU_HEAPS 1
#endif
#ifndef MEMENTO_CPU_MAX
    #define MEMENTO_CPU_MAX 256
#endif

#ifndef MEMENTO_VA_RESERVE
    #define MEMENTO_VA_RESERVE (256ull * 1024ull * 1024ull)
#endif
#ifndef MEMENTO_GLOBAL_PAGE_CACHE
    #define MEMENTO_GLOBAL_PAGE_CACHE 64
#endif

/* Compile-time ISA for miss-path SIMD. Never cpuid on the hit path. */
#ifndef MEMENTO_SIMD_AVX512
    #if defined(__AVX512F__) && defined(__AVX512BW__)
        #define MEMENTO_SIMD_AVX512 1
    #else
        #define MEMENTO_SIMD_AVX512 0
    #endif
#endif
#ifndef MEMENTO_SIMD_AVX2
    #if defined(__AVX2__) && !MEMENTO_SIMD_AVX512
        #define MEMENTO_SIMD_AVX2 1
    #elif defined(__AVX2__)
        #define MEMENTO_SIMD_AVX2 1
    #else
        #define MEMENTO_SIMD_AVX2 0
    #endif
#endif
#ifndef MEMENTO_SIMD_NEON
    #if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !MEMENTO_SIMD_AVX2 && !MEMENTO_SIMD_AVX512
        #define MEMENTO_SIMD_NEON 1
    #else
        #define MEMENTO_SIMD_NEON 0
    #endif
#endif

/* How many blocks to carve into the freelist on a size-class cache miss */
#ifndef MEMENTO_REFILL_BATCH
    #define MEMENTO_REFILL_BATCH 32
#endif

/* Page-run tier: sizes in (MEMENTO_MAX_SIZE_CLASS, MEMENTO_PAGE_RUN_MAX]
 * are rounded to whole 4 KiB pages and cached per page count, so repeatedly
 * reallocating a ~100 KiB buffer does not donate a fresh VMA to the kernel
 * every time. Above MEMENTO_PAGE_RUN_MAX, exact-size mmap caching applies. */
#ifndef MEMENTO_PAGE_RUN_MAX
    #define MEMENTO_PAGE_RUN_MAX (256u * 1024u)
#endif
#ifndef MEMENTO_PAGE_RUN_CACHE_PER_CLASS
    #define MEMENTO_PAGE_RUN_CACHE_PER_CLASS 4
#endif

/* Milliseconds an empty span keeps its pages before they are discarded.
 * This is the mimalloc lesson: an immediately-emptied span is very often
 * about to be refilled (burst traffic), and discarding it eagerly turns the
 * next refill into 512 page faults at ~1 us each — the madvise ping-pong
 * that tanks mixed workloads. So reclamation is delayed: reclaim() stamps a
 * deadline, and each subsequent reclaim discards whatever expired. Churn
 * (span recycled within the window) never pays a single fault; a burst that
 * stays freed for MEMENTO_SPAN_PURGE_MS gets its RSS back. Set to 0 for
 * eager discard, or call memento_heap_release_caches() to force it. */
#ifndef MEMENTO_SPAN_PURGE_MS
    #define MEMENTO_SPAN_PURGE_MS 10
#endif

/* Backstop for the delayed purge: a span parked this deep is discarded
 * regardless of age, so a burst of thousands of empty spans within one
 * purge window cannot retain unbounded RSS. 16 spans = 32 MiB per heap. */
#ifndef MEMENTO_SPAN_CACHE_MAX
    #define MEMENTO_SPAN_CACHE_MAX 16
#endif

/* Page runs cached without MADV_FREE per page-count class. Cached runs past
 * the hot set get MADV_FREE so the kernel can reclaim them under pressure;
 * the hot ones stay fully backed because a same-size allocation is the
 * overwhelmingly likely next event (churn), and re-faulting is a syscall
 * plus page-table work we would rather skip. */
#ifndef MEMENTO_PAGE_RUN_HOT
    #define MEMENTO_PAGE_RUN_HOT 2
#endif

/* Huge-object cache: reuse by page count before mmap/munmap */
#ifndef MEMENTO_LARGE_CACHE_SLOTS
    #define MEMENTO_LARGE_CACHE_SLOTS 8
#endif
/* Do not cache huge mappings bigger than this (user size) */
#ifndef MEMENTO_LARGE_CACHE_MAX_SIZE
    #define MEMENTO_LARGE_CACHE_MAX_SIZE (4u * 1024u * 1024u)
#endif
#define MEMENTO_LARGE_CACHE_MAX_PAGES (MEMENTO_LARGE_CACHE_MAX_SIZE >> 12)

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

/* Instrumented proxies (cookies, owner, site ids, leak reports). Defaults to
 * MEMENTO_DEBUG; costs a side-table entry per live allocation. Set to 0 for a
 * production proxy that is just two function pointers. */
#ifndef MEMENTO_PROXY_DEBUG
    #define MEMENTO_PROXY_DEBUG MEMENTO_DEBUG
#endif

/* Guard pages at arena high-water marks (mprotect PROT_NONE past the live
 * region). Defaults on in debug builds; POSIX + Windows only. */
#ifndef MEMENTO_ARENA_GUARD
    #define MEMENTO_ARENA_GUARD MEMENTO_DEBUG
#endif

/* Allocation statistics. 1 (default): counters maintained on every op and
 * memento_thread_heap_report() is fully populated. 0: counters compile out
 * of the alloc/free hit path entirely — reports show zeros, and you get the
 * "load head, store next, return" path with nothing attached. */
#ifndef MEMENTO_STATS
    #define MEMENTO_STATS 1
#endif

/* Recycle thread heaps: on thread exit the heap is parked, and the next
 * thread on this slot adopts it (no fresh mmap storm in worker pools).
 * Disable with -DMEMENTO_HEAP_RECYCLING=0 to restore retire-forever. */
#ifndef MEMENTO_HEAP_RECYCLING
    #define MEMENTO_HEAP_RECYCLING 1
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

/* Valgrind client requests (MALLOCLIKE/FREELIKE) so exact-size mode is not
 * invisible to valgrind --leak-check. Auto-enabled when the valgrind headers
 * are available; zero cost at runtime unless actually running under valgrind
 * (every request is gated on RUNNING_ON_VALGRIND). */
#ifndef MEMENTO_ENABLE_VALGRIND
    #if defined(__has_include)
        #if __has_include(<valgrind/memcheck.h>)
            #define MEMENTO_ENABLE_VALGRIND 1
        #else
            #define MEMENTO_ENABLE_VALGRIND 0
        #endif
    #else
        #define MEMENTO_ENABLE_VALGRIND 0
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
 * Used for the per-heap foreign-free MPSC stack head and the park list. */
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
typedef struct memento_proxy memento_proxy_t;

/* Size classes (29): 16-byte steps through 256 B, then ~28% steps to 8 KiB.
 * 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512,
 * 640, 768, 896, 1024, 1280, 1536, 2048, 2560, 3200, 4096, 5120, 6144,
 * 7680, 8192. Generated LUT below; regenerate with tools/gen_lut.py. */
#define MEMENTO_SIZE_CLASS_COUNT 29

/* Thread-local heap statistics (for debugging/monitoring) */
typedef struct {
    size_t alloc_count;         /* Allocation calls served */
    size_t free_count;          /* Frees completed by the owner */
    size_t foreign_free_count;  /* Frees pushed by other threads, drained here */
    size_t bytes_allocated;     /* Cumulative bytes allocated (class sizes) */
    size_t bytes_freed;         /* Cumulative bytes freed */
    size_t bytes_live;          /* allocated - freed, right now */
    size_t bytes_peak;          /* High-water mark of bytes_live */
    size_t span_count;          /* 2 MiB spans currently mapped */
    size_t spans_reclaimed;     /* Empty spans returned to the kernel (cumulative) */
    size_t page_run_cached;     /* Page-run blocks sitting in the cache */
    size_t huge_cached;         /* Huge blocks sitting in the exact-size cache */
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

/* Get thread-local heap. Creates one on first call — or adopts a parked one
 * (see MEMENTO_HEAP_RECYCLING), so short-lived worker threads do not pay an
 * mmap storm. Cached in TLS. */
memento_thread_heap_t* memento_thread_heap_get(void);

/* Allocate from thread-local heap (owning thread only, non-locking).
 * memento_thread_heap_free may be called from ANY thread: foreign frees are
 * pushed onto the heap's lock-free MPSC stack and reclaimed by the owner.
 * EXACT_SIZE mode: the exact allocation size must be passed to free.
 * SIZED mode: size is ignored at free; the header is authoritative. */
MEMENTO_ATTR_MALLOC MEMENTO_ATTR_ALLOC_SIZE(2) MEMENTO_ATTR_FLATTEN
void* memento_thread_heap_alloc(memento_thread_heap_t* heap, size_t size);
void memento_thread_heap_free(memento_thread_heap_t* heap, void* ptr, size_t size);
void* memento_thread_heap_realloc(memento_thread_heap_t* heap, void* ptr,
                                   size_t old_size, size_t new_size);

/* Aligned allocation (exact-size contract). alignment must be a power of two;
 * values <= 16 are free (every block is at least 16-byte aligned). Larger
 * alignments carry a small back-pointer header; free with
 * memento_thread_heap_free_aligned passing the SAME size and alignment. */
MEMENTO_ATTR_MALLOC MEMENTO_ATTR_ALLOC_SIZE(2) MEMENTO_ATTR_ALLOC_ALIGN(3)
void* memento_thread_heap_alloc_aligned(memento_thread_heap_t* heap,
                                        size_t size, size_t alignment);
void memento_thread_heap_free_aligned(memento_thread_heap_t* heap, void* ptr,
                                      size_t size, size_t alignment);

/* Drain pending foreign deallocations (owning thread only; cheap when empty -
 * a single relaxed load). Call from scheduler idle paths. */
void memento_thread_heap_flush(memento_thread_heap_t* heap);

void memento_thread_heap_stats(memento_thread_heap_t* heap, memento_heap_stats_t* stats);

/* Human-readable heap report: allocs/frees/foreign frees, bytes live and
 * peak, top size classes, registered proxy sites, retired/parked state with
 * the number of blocks still parked on the foreign stack. Call from the
 * owning thread (or at shutdown after joining everyone). */
void memento_thread_heap_report(memento_thread_heap_t* heap, FILE* out);

/* Best-effort report across every registered heap (takes the registry lock;
 * do not call from a signal handler if a heap op may be in flight — the
 * malloc shim's SIGUSR1 dump accepts that risk, you get to choose). */
void memento_report_all(FILE* out);

/* NUMA node this heap first-touched spans on, or -1 if unknown / disabled */
int memento_thread_heap_numa_node(const memento_thread_heap_t* heap);

/* Park the calling thread's heap for reuse: flush foreign frees, detach TLS,
 * and park it. The next memento_thread_heap_get() / memento_heap_adopt() on
 * any thread may adopt it. This is the thread-pool pattern: workers park on
 * exit instead of retiring, so heap setup (spans, caches) is paid once per
 * pool slot instead of once per thread lifetime. */
void memento_heap_park(void);

/* Try to adopt a parked heap for the calling thread. Returns the adopted
 * heap, or NULL if none was parked (call memento_thread_heap_get() to create
 * a fresh one in that case). */
memento_thread_heap_t* memento_heap_adopt(void);

/* Return every cached span and large mapping to the OS, right now. The
 * delayed purge (MEMENTO_SPAN_PURGE_MS) is the right default for throughput,
 * but it is deliberately fuzzy about WHEN memory goes back; this is the
 * deterministic alternative. Call it before parking a heap, after a known
 * memory spike, or whenever RSS has to drop on your schedule instead of the
 * allocator's. Safe to call with live blocks outstanding: only caches and
 * empty spans are released. Owner thread only. */
void memento_heap_release_caches(memento_thread_heap_t* heap);

/* Register fork handlers (POSIX only; returns pthread_atfork's result, or 0
 * on Windows where it is a no-op). In the fork child EVERY heap is retired
 * and the thread starts with no heap at all: allocating on the parent's
 * copy-on-write spans would duplicate every page the parent keeps using, so
 * the child's next allocation creates a fresh heap with fresh private spans.
 * Freeing pre-fork pointers in the child stays correct — ownership routes
 * through the span header to the retired heap, reclaimed at shutdown.
 * Fork from a quiescent point: foreign frees caught mid-push at fork time
 * are lost in the child. */
int memento_atfork_register(void);

/* Monotonic counter bumped in every fork child. Embedders that cache heap
 * pointers can detect a fork by comparing generations (always 0 where there
 * is no fork). */
unsigned long memento_fork_generation_current(void);

/* Runtime knobs. op is MEMENTO_CTL_*; arg is in/out depending on op.
 * Returns 0 on success, -1 on unknown op / bad arg. */
#define MEMENTO_CTL_GET_TCACHE_DEPTH  1  /* arg: size_t* */
#define MEMENTO_CTL_SET_TCACHE_DEPTH  2  /* arg: const size_t* */
#define MEMENTO_CTL_TRIM              3  /* arg: ignored; same as malloc_trim */
int memento_ctl(int op, void* arg);

/* mallinfo2-shaped snapshot across every registered heap. */
typedef struct memento_mallinfo_s {
    size_t arena;     /* bytes mapped from the OS (spans + large) */
    size_t ordblks;   /* free span/page cache entries */
    size_t smblks;    /* unused (0) */
    size_t hblks;     /* huge / page-run mappings cached */
    size_t hblkhd;    /* bytes in those mappings */
    size_t usmblks;   /* unused (0) */
    size_t fsmblks;   /* unused (0) */
    size_t uordblks;  /* bytes live in user hands */
    size_t fordblks;  /* bytes sitting in caches */
    size_t keepcost;  /* bytes malloc_trim can return */
} memento_mallinfo_t;

memento_mallinfo_t memento_mallinfo(void);
void memento_malloc_trim(void);

/* ============================================================================
 * Sized API - free() without a size, always available
 * ============================================================================
 * These carry a 16-byte header per allocation (size + back-offset + kind) so
 * free needs nothing but the pointer. Ownership is resolved from the pointer
 * itself (span-aligned lookup for small blocks, an allocation header for
 * large ones), which means memento_free(p) from the WRONG thread still routes
 * the block home through the owner's MPSC stack. This is the family you point
 * third-party C code at when it "doesn't know the size".
 */

/* Heap-scoped */
MEMENTO_ATTR_MALLOC MEMENTO_ATTR_ALLOC_SIZE(2)
void*  memento_heap_malloc(memento_thread_heap_t* heap, size_t size);
MEMENTO_ATTR_MALLOC MEMENTO_ATTR_ALLOC_SIZE(2, 3)
void*  memento_heap_calloc(memento_thread_heap_t* heap, size_t count, size_t size);
void*  memento_heap_realloc(memento_thread_heap_t* heap, void* ptr, size_t new_size);
void   memento_heap_mfree(memento_thread_heap_t* heap, void* ptr);
MEMENTO_ATTR_MALLOC MEMENTO_ATTR_ALLOC_ALIGN(2) MEMENTO_ATTR_ALLOC_SIZE(3)
void*  memento_heap_aligned_alloc(memento_thread_heap_t* heap,
                                  size_t alignment, size_t size);
size_t memento_heap_usable_size(const void* ptr);

/* Global (calling thread's heap) — a drop-in malloc family. */
MEMENTO_ATTR_MALLOC MEMENTO_ATTR_ALLOC_SIZE(1)
void*  memento_malloc(size_t size);
MEMENTO_ATTR_MALLOC MEMENTO_ATTR_ALLOC_SIZE(1, 2)
void*  memento_calloc(size_t count, size_t size);
void*  memento_realloc(void* ptr, size_t new_size);
void   memento_free(void* ptr);
MEMENTO_ATTR_MALLOC MEMENTO_ATTR_ALLOC_ALIGN(1) MEMENTO_ATTR_ALLOC_SIZE(2)
void*  memento_aligned_alloc(size_t alignment, size_t size);
int    memento_posix_memalign(void** out, size_t alignment, size_t size);
size_t memento_usable_size(const void* ptr);

/* The class size an allocation of `size` would land in (sralloc's
 * alloc_with_size idea: sometimes the allocator can hand you more than you
 * asked for — this tells you how much). */
size_t memento_good_size(size_t size);

/* ============================================================================
 * Proxy API - one instrumented front-end for every allocator
 * ============================================================================
 * A proxy wraps any memento allocator behind two function pointers. Every
 * live allocation gets a side-table entry (user pointer, raw block, free
 * size, requested size, file/line) — that is what makes memento_proxy_free()
 * correct without a size argument in every build, and what lets
 * memento_proxy_report() tell you exactly who is leaking. A proxy is
 * something you opt into for observability; the table costs one hash
 * lookup per call. MEMENTO_PROXY_DEBUG=1 (the default in MEMENTO_DEBUG
 * builds) adds per-entry cookies, owner-thread ids, and hard failures on
 * double-alloc/untracked-free; release builds warn and skip instead.
 */

#define MEMENTO_PROXY_COOKIE    0x01u /* validate an 8-byte cookie on free */
#define MEMENTO_PROXY_OWNER     0x02u /* record the allocating thread id */
#define MEMENTO_PROXY_SITE      0x04u /* record a file/line site id */
#define MEMENTO_PROXY_WATERMARK 0x08u /* track live/peak bytes */
#define MEMENTO_PROXY_TRACE     0x10u /* fprintf every alloc/free (very loud) */
#define MEMENTO_PROXY_DEBUG_ALL \
    (MEMENTO_PROXY_COOKIE | MEMENTO_PROXY_OWNER | MEMENTO_PROXY_SITE | \
     MEMENTO_PROXY_WATERMARK)

struct memento_proxy {
    void* (*alloc)(void* impl, size_t size, size_t align);
    void  (*free_)(void* impl, void* ptr, size_t size);
    void* impl;
    uint32_t flags;
    uint32_t _pad;
};

/* Wrap an existing allocator. flags are MEMENTO_PROXY_*; pass
 * MEMENTO_PROXY_DEBUG_ALL for the full debug treatment. The proxy does not
 * own the wrapped allocator — destroy it yourself, then the proxy. */
memento_proxy_t* memento_proxy_wrap_heap(memento_thread_heap_t* heap, uint32_t flags);
memento_proxy_t* memento_proxy_wrap_pool(memento_pool_t* pool, uint32_t flags);
memento_proxy_t* memento_proxy_wrap_arena(memento_arena_t* arena, uint32_t flags);
memento_proxy_t* memento_proxy_wrap_stack(memento_stack_t* stack, uint32_t flags);
memento_proxy_t* memento_proxy_wrap_slab(memento_slab_t* slab, uint32_t flags);
void memento_proxy_destroy(memento_proxy_t* proxy);

/* allocate through a proxy; the macros capture the call site (file/line). */
void* memento_proxy_alloc_site(memento_proxy_t* proxy, size_t size,
                               size_t align, const char* file, int line);
void  memento_proxy_free(memento_proxy_t* proxy, void* ptr);

uint64_t memento_site_id(const char* file, int line);
#define memento_proxy_alloc(proxy, size) \
    memento_proxy_alloc_site((proxy), (size), 0, __FILE__, __LINE__)
#define memento_proxy_alloc_aligned(proxy, size, align) \
    memento_proxy_alloc_site((proxy), (size), (align), __FILE__, __LINE__)

/* Leak/usage report: outstanding allocations grouped by site, live and peak
 * bytes, alloc/free totals. For an arena proxy, call it right before
 * memento_arena_reset(): "outstanding" is everything allocated since the last
 * reset that was never proxy-freed. */
void memento_proxy_report(memento_proxy_t* proxy, FILE* out);
size_t memento_proxy_outstanding_count(memento_proxy_t* proxy);
size_t memento_proxy_outstanding_bytes(memento_proxy_t* proxy);

/* ============================================================================
 * Pool Allocator API - Fixed-size object pools
 * ============================================================================
 * A pool starts with one block of `capacity` objects and GROWS by doubling
 * (bounded) when it runs dry — alloc fails only when the heap is out of
 * memory. OWNER THREAD ONLY: no internal locking. Debug builds enforce the
 * owner thread and detect double-frees; release builds trust the caller.
 */

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
/* Guarded arena: every block gets a PROT_NONE page at its high-water end, so
 * a bump overflow segfaults at the guard instead of corrupting whatever lives
 * next door (say, a fiber stack). POSIX and Windows. */
memento_arena_t* memento_arena_create_guarded(size_t initial_capacity,
                                               memento_thread_heap_t* heap);
void memento_arena_destroy(memento_arena_t* arena);

/* alignment must be a power of two (or 0, treated as 1) */
void* memento_arena_alloc(memento_arena_t* arena, size_t size, size_t alignment);

/* Save/restore for temporary allocations. Restore rewinds the recorded
 * block's top AND returns every block allocated after the save point to the
 * thread heap — a save/restore-heavy workload (compilers, parsers, eval
 * loops) no longer ratchets memory upward. */
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
 * ============================================================================
 * OWNER THREAD ONLY: no internal locking. Debug builds enforce the owner
 * thread and validate that pop() takes the most recent push (LIFO).
 */

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
 * ============================================================================
 * OWNER THREAD ONLY: no internal locking. Debug builds enforce the owner
 * thread and detect double-frees within the bounded per-class caches.
 */

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

#if MEMENTO_SIMD_AVX512 || MEMENTO_SIMD_AVX2
    #include <immintrin.h>
#endif
#if MEMENTO_SIMD_NEON
    #include <arm_neon.h>
#endif

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

#if (MEMENTO_SPAN_SIZE & (MEMENTO_SPAN_SIZE - 1)) != 0
    #error "MEMENTO_SPAN_SIZE must be a power of two (span-aligned owner lookup)"
#endif
#if (MEMENTO_PAGE_RUN_MAX & 4095u) != 0
    #error "MEMENTO_PAGE_RUN_MAX must be a multiple of 4096"
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

    /* Registry lock: initialized inside the InitOnce callback (see
     * memento_init) — the once, not a racy flag, is what serializes it. */
    static CRITICAL_SECTION memento_registry_lock;
    static INIT_ONCE memento_init_once = INIT_ONCE_STATIC_INIT;

    #define MEMENTO_REGISTRY_LOCK()   EnterCriticalSection(&memento_registry_lock)
    #define MEMENTO_REGISTRY_UNLOCK() LeaveCriticalSection(&memento_registry_lock)

    /* FLS for TLS destructor on thread exit */
    static DWORD memento_fls_index = FLS_OUT_OF_INDEXES;
    static int memento_fls_ready = 0;

#else /* POSIX */
    #include <sys/mman.h>
    #include <unistd.h>
    #include <pthread.h>
    #include <errno.h>
    #include <time.h> /* clock_gettime(CLOCK_MONOTONIC) — the purge clock */
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
    static int memento_atfork_registered = 0;
    static pthread_once_t memento_init_once = PTHREAD_ONCE_INIT;
#endif

/* ============================================================================
 * Size Classes - branchless LUT (16-byte buckets; classes use 16B steps)
 * ============================================================================ */

static const size_t memento_size_classes[MEMENTO_SIZE_CLASS_COUNT] = {
    32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320,
    384, 448, 512, 640, 768, 896, 1024, 1280, 1536, 2048, 2560,
    3200, 4096, 5120, 6144, 7680, 8192
};

/* Index = (size + 15) >> 4 for size in 1..8192 (indices 1..512).
 * MACHINE-GENERATED from memento_size_classes above — regenerate with
 * tools/gen_lut.py after touching the class table, and lean on the
 * lut_exhaustive test. A hand-edited tail once mapped 4096 to a 768-byte
 * class; that class of bug is why this table is not typed by hand. */
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
    22, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 24, 24, 24, 24, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
    25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
    25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
    25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
    25, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
    27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
    27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
    27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
    27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
    27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
    27, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28
};

/* Returned by memento_size_class_for for requests above MEMENTO_MAX_SIZE_CLASS
 * (those belong to the page-run tier — they have no size class). */
#define MEMENTO_SIZE_CLASS_NONE ((size_t)-1)

/* (size+15)>>4 for size in 0..256 → indices 0..16. Same bytes as the
 * head of memento_sc_lut; a 17-entry table stays in L1 with the compare. */
static const uint8_t memento_sc_lut_small[17] = {
    0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 7, 8, 8, 9, 9, 10, 10
};

MEMENTO_FORCE_INLINE size_t memento_size_class_for(size_t size) {
    if (MEMENTO_UNLIKELY(size == 0)) return 0;
    if (MEMENTO_UNLIKELY(size > MEMENTO_MAX_SIZE_CLASS)) {
        return MEMENTO_SIZE_CLASS_NONE;
    }
    if (size <= 256) {
        return (size_t)memento_sc_lut_small[(size + 15) >> 4];
    }
    return (size_t)memento_sc_lut[(size + 15) >> 4];
}

MEMENTO_FORCE_INLINE size_t memento_size_class_to_size(size_t sc) {
    return (sc < MEMENTO_SIZE_CLASS_COUNT)
        ? memento_size_classes[sc]
        : 0; /* MEMENTO_SIZE_CLASS_NONE or garbage: no such class */
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

MEMENTO_FORCE_INLINE uint8_t memento_log2_pow2(size_t x) {
    uint8_t n = 0;
    while (x > 1) { x >>= 1; n++; }
    return n;
}

/* ============================================================================
 * Internal Thread Cache
 * ============================================================================ */

typedef struct memento_cache_node_s {
    struct memento_cache_node_s* next;
} memento_cache_node_t;

/* One cache line per size class — eliminates false sharing when multiple
 * threads allocate different classes, and keeps the hit path to a single
 * dirty line. Per-class liveness stats live in heap->class_live[] (cold
 * region): they are written on every alloc/free, and dirtying the freelist
 * line with telemetry on the thing we claimed was the fast path is how you
 * stay 2x behind mimalloc. */
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
 * size class is 32 bytes, so every block can hold it.
 *
 * Layout matters: `next` at offset 0 doubles as the local freelist link;
 * `tag` at offset 8 is the debug double-free cookie's home (see the canary
 * section — foreign pushes stamp it, flush verifies it); `size` at offset
 * 16 stays clear of both. Overlaying `size` on the cookie slot (as v3.0
 * did) meant a foreign free silently erased the very evidence the cookie
 * was meant to preserve. */
typedef struct memento_foreign_node_s {
    struct memento_foreign_node_s* next; /* offset 0  */
    uint64_t tag;                        /* offset 8: debug cookie slot */
    size_t size;                         /* offset 16 */
} memento_foreign_node_t;

/* 2 MiB OS span, aligned to its own size so any small block's span (and thus
 * owning heap) is one AND away. Bump-carved into size-class blocks.
 *
 * A span serves exactly ONE size class (sc) — that is what makes bounded
 * reclamation possible. `live` counts blocks currently in user hands
 * (incremented when a block leaves the freelist/refill, decremented when a
 * free returns it; foreign frees count down at drain time, so an in-flight
 * block still counts as live). When live hits 0 every block of the span sits
 * on that one class's freelist, and unlinking them is one O(limit) walk. The
 * span then goes back with MADV_DONTNEED (RSS returns to the kernel; the
 * mapping stays so memento_span_of keeps working for foreign frees) and is
 * reused by the next refill of any class. */
typedef struct memento_span_s {
    struct memento_span_s* next;      /* heap->span_list (all spans) */
    struct memento_span_s* free_next; /* heap->span_free (reclaimed spans) */
    struct memento_thread_heap_s* owner;
    size_t used;
    size_t capacity;
    uint32_t carved;     /* blocks carved since (re)assignment (debug/stats) */
    int32_t live;        /* blocks currently in user hands — the reclaim trigger */
    int32_t sc;          /* owning size class; -1 while unassigned */
    uint8_t reclaimed;   /* 0 active, 1 free-listed (pages kept), 2 free-listed (MADV_DONTNEED'd) */
    uint8_t _pad[3];
    uint64_t purge_at;   /* CLOCK_MONOTONIC ms deadline for discarding this span's pages */
    /* Data-region offset from which bytes are guaranteed kernel-zero.
     * Fresh mmap: 0. After MADV_DONTNEED reuse: from the first discarded
     * page. After an in-place bump restart or a retained-span recycle: the
     * old high-water mark. calloc skips its memset when a block is carved
     * at or past zero_from. */
    size_t zero_from;
    /* Flexible data region starts after header; whole span is MEMENTO_SPAN_SIZE */
    char data[1];
} memento_span_t;

#define MEMENTO_SPAN_HEADER offsetof(memento_span_t, data)

/* The span that owns a small-block pointer (spans are self-aligned). */
MEMENTO_FORCE_INLINE memento_span_t* memento_span_of(const void* ptr) {
    return (memento_span_t*)((uintptr_t)ptr & ~(uintptr_t)(MEMENTO_SPAN_SIZE - 1));
}

#if MEMENTO_PAGE_SIZE
#if (MEMENTO_SPAN_SIZE % MEMENTO_PAGE_SIZE) != 0
    #error "MEMENTO_PAGE_SIZE must divide MEMENTO_SPAN_SIZE"
#endif
#define MEMENTO_PAGES_PER_SPAN (MEMENTO_SPAN_SIZE / MEMENTO_PAGE_SIZE)

/* 64 KiB page header: lives at the base of each data page (not page 0,
 * which holds the span header). Hit-path nused lives on this line, not
 * 2 MiB away on the span. Foreign frees CAS onto thread_free. */
#define MEMENTO_PAGE_BMP_WORDS 32u /* 2048 bits = 256 B, 32 B class worst case */

typedef struct memento_page {
    void* free;
    uint16_t nfree;
    uint16_t nused;
    uint8_t sc;
    uint8_t state;
    uint16_t nbits;    /* valid occupancy bits */
    memento_atomic_ptr_t thread_free;
    struct memento_page* next_partial;
    MEMENTO_ALIGNED(16) uint64_t bmp[MEMENTO_PAGE_BMP_WORDS];
} memento_page_t;

MEMENTO_FORCE_INLINE memento_page_t* memento_page_of(const void* ptr) {
    return (memento_page_t*)((uintptr_t)ptr & ~(uintptr_t)(MEMENTO_PAGE_SIZE - 1));
}

MEMENTO_FORCE_INLINE size_t memento_page_payload_off(void) {
    return (sizeof(memento_page_t) + 15u) & ~(size_t)15u;
}

MEMENTO_FORCE_INLINE void memento_bmp_set(uint64_t* b, unsigned bit) {
    b[bit >> 6] |= (1ull << (bit & 63u));
}

MEMENTO_FORCE_INLINE void memento_bmp_clear(uint64_t* b, unsigned bit) {
    b[bit >> 6] &= ~(1ull << (bit & 63u));
}

MEMENTO_FORCE_INLINE int memento_page_idx_of(memento_page_t* pg, void* ptr, size_t bs) {
    size_t off = (size_t)((char*)ptr - (char*)pg) - memento_page_payload_off();
    return (int)(off / bs);
}

typedef struct {
    uint32_t top; /* slot[0] sentinel; never popped. Alloc: --top. Free: top++ */
    void* slot[MEMENTO_TCACHE_DEPTH_MAX];
} memento_tcache_bin_t;
#endif

/* Meta header at the start of every page-run / huge mapping (32 bytes, so the
 * user pointer at base+32 keeps 16-byte alignment on a page-aligned base). */
#define MEMENTO_LARGE_MAGIC 0x4D454D4C41524745ull /* "MEMLARGE" */
typedef struct {
    uint64_t magic;
    uint64_t pages;      /* mapping length in pages — the free path's truth */
    uint64_t user_size;  /* requested size at alloc (informational) */
    struct memento_thread_heap_s* owner;
} memento_large_meta_t;

#define MEMENTO_LARGE_META_SIZE 32u /* sizeof(memento_large_meta_t) */

/* Huge-object cache slot (> MEMENTO_PAGE_RUN_MAX), keyed by page count */
typedef struct {
    void* ptr;    /* user pointer (past meta header) */
    size_t pages; /* mapping length in pages — the cache key */
} memento_large_slot_t;

/* Page-run free-list node: lives in the meta area of a cached run. */
typedef struct memento_page_run_node_s {
    struct memento_page_run_node_s* next;
} memento_page_run_node_t;

#define MEMENTO_PAGE_RUN_CLASSES (MEMENTO_PAGE_RUN_MAX / 4096u)

struct memento_thread_heap_s {
    /* Size-class free lists: each entry is its own cache line */
    MEMENTO_ALIGNED(MEMENTO_CACHE_LINE_SIZE)
    memento_size_class_cache_t caches[MEMENTO_SIZE_CLASS_COUNT];

    /* Foreign deallocation stack (MPSC) — isolated cache line */
    MEMENTO_ALIGNED(MEMENTO_CACHE_LINE_SIZE) memento_atomic_ptr_t foreign_head;
    char foreign_pad[MEMENTO_CACHE_LINE_SIZE - sizeof(memento_atomic_ptr_t)];

    /* Cold fields (owner thread only, rarely shared) */
    MEMENTO_ALIGNED(MEMENTO_CACHE_LINE_SIZE) memento_heap_stats_t stats;
#if MEMENTO_STATS
    /* Per-class live block counts (the report's top-classes table). Kept
     * off the freelist cache lines; bytes are derivable: live * class size. */
    uint64_t class_live[MEMENTO_SIZE_CLASS_COUNT];
#endif
    uint64_t thread_id;
    struct memento_thread_heap_s* registry_next;
    struct memento_thread_heap_s* park_next;

    /* Span bump allocators: one partial span per size class (spans are
     * single-class so that empty-span reclamation can unlink a span's
     * blocks from exactly one freelist), plus a reuse stack of reclaimed
     * spans. Parked spans keep their pages until their purge deadline
     * (MEMENTO_SPAN_PURGE_MS) or until parked deeper than
     * MEMENTO_SPAN_CACHE_MAX; recycling a young span costs no page faults. */
    memento_span_t* span_partial[MEMENTO_SIZE_CLASS_COUNT];
    memento_span_t* span_free;
    memento_span_t* span_list;
    uint32_t span_free_count;
#if MEMENTO_PAGE_SIZE
    memento_page_t* active[MEMENTO_SIZE_CLASS_COUNT];
    memento_tcache_bin_t tcache[MEMENTO_SIZE_CLASS_COUNT];
#endif

    /* Page-run cache: free lists by page count (1..MEMENTO_PAGE_RUN_CLASSES) */
    memento_page_run_node_t* page_runs[MEMENTO_PAGE_RUN_CLASSES];
    uint32_t page_run_count[MEMENTO_PAGE_RUN_CLASSES];

    /* Huge-object cache (exact size match) */
    memento_large_slot_t large_cache[MEMENTO_LARGE_CACHE_SLOTS];
    uint32_t large_count;

    /* Proxies wrapped around this heap (debug builds; owner thread only) */
    struct memento_proxy_state_s* proxy_list;

    /* NUMA node observed at heap create (-1 unknown) */
    int32_t numa_node;

    uint8_t initialized;
    uint8_t retired;
    uint8_t parked;
    uint8_t _pad[1];
};

/* ============================================================================
 * Thread-local Storage
 * ============================================================================ */

static MEMENTO_TLS memento_thread_heap_t* memento_tls_heap = NULL;
static MEMENTO_TLS uint64_t memento_cached_now_ms = 0;
static uint32_t memento_rt_tcache_depth = MEMENTO_TCACHE_DEPTH;
#if MEMENTO_CPU_HEAPS
static memento_thread_heap_t* memento_cpu_heaps[MEMENTO_CPU_MAX];
static unsigned memento_current_cpu(void);
#endif

MEMENTO_FORCE_INLINE uint32_t memento_tcache_depth(void) {
    uint32_t d = memento_rt_tcache_depth;
    if (d < 2) d = 2;
    if (d > MEMENTO_TCACHE_DEPTH_MAX) d = MEMENTO_TCACHE_DEPTH_MAX;
    return d;
}
static memento_thread_heap_t* memento_heap_registry = NULL;
static memento_thread_heap_t* memento_parked_heaps = NULL;
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

#if MEMENTO_ENABLE_VALGRIND
    #include <valgrind/memcheck.h>
    /* Gate on RUNNING_ON_VALGRIND so non-valgrind runs pay one predictable
     * branch (a constant 0), not the client-request encoding. */
    #define MEMENTO_VALGRIND_MALLOCLIKE(p, n) do { \
        if (RUNNING_ON_VALGRIND) { \
            VALGRIND_MALLOCLIKE_BLOCK((p), (n), 0, 1); \
        } } while (0)
    #define MEMENTO_VALGRIND_FREELIKE(p) do { \
        if (RUNNING_ON_VALGRIND) { \
            VALGRIND_FREELIKE_BLOCK((p), 0); \
        } } while (0)
#else
    #define MEMENTO_VALGRIND_MALLOCLIKE(p, n) do { (void)(p); (void)(n); } while (0)
    #define MEMENTO_VALGRIND_FREELIKE(p)      ((void)0)
#endif

/* Used by MEMENTO_DEBUG paths and by the proxy's own debug layer
 * (MEMENTO_PROXY_DEBUG may be on in an otherwise-release build). */
#if MEMENTO_DEBUG || MEMENTO_PROXY_DEBUG
static void memento_debug_fail(const char* msg) {
    fputs("memento: ", stderr);
    fputs(msg, stderr);
    fputc('\n', stderr);
    abort();
}
#endif

#if MEMENTO_DEBUG
    /* Double-free cookie at block offset 8: a 64-bit value derived from the
     * block's own address, so a user payload colliding with it is a 2^-64
     * event rather than the 2^-32 one v3.0 shipped. The state machine is
     * one-way: free stamps the cookie (and screams if it was already there
     * = double-free); alloc clears a stamped cookie and otherwise leaves the
     * word alone — it may legitimately hold a sized header or fresh span
     * garbage, and neither is alloc's business. Blocks freshly carved at
     * refill are pre-stamped so their first free is indistinguishable from
     * any other. Foreign pushes stamp through memento_debug_on_foreign_free
     * and flush un-stamps before handing the block back to the local path. */
    #define MEMENTO_FREE_COOKIE  0xF4EEF4EEA110C8EDull
    #define MEMENTO_LIVE_COOKIE  0x5AFE5AFE00DD00DDull

    MEMENTO_FORCE_INLINE uint64_t memento_debug_cookie(const void* ptr) {
        return MEMENTO_FREE_COOKIE ^ (uint64_t)(uintptr_t)ptr;
    }

    MEMENTO_FORCE_INLINE void memento_debug_on_free(const void* ptr) {
        uint64_t* slot = (uint64_t*)((char*)ptr + sizeof(void*));
        if (MEMENTO_UNLIKELY(*slot == memento_debug_cookie(ptr))) {
            memento_debug_fail("double-free detected");
        }
        *slot = memento_debug_cookie(ptr);
    }

    MEMENTO_FORCE_INLINE void memento_debug_on_alloc(const void* ptr) {
        uint64_t* slot = (uint64_t*)((char*)ptr + sizeof(void*));
        if (*slot == memento_debug_cookie(ptr)) {
            *slot = MEMENTO_LIVE_COOKIE;
        }
    }

    MEMENTO_FORCE_INLINE void memento_debug_mark_free(const void* ptr) {
        uint64_t* slot = (uint64_t*)((char*)ptr + sizeof(void*));
        *slot = memento_debug_cookie(ptr);
    }

    /* Foreign push stamps the cookie too — a block freed twice from two
     * threads is still a double-free. Flush verifies the stamp before it
     * trusts node->size. */
    MEMENTO_FORCE_INLINE void memento_debug_on_foreign_free(const void* ptr) {
        memento_debug_on_free(ptr);
    }

    MEMENTO_FORCE_INLINE void memento_debug_on_foreign_drain(const void* ptr) {
        uint64_t* slot = (uint64_t*)((char*)ptr + sizeof(void*));
        if (MEMENTO_UNLIKELY(*slot != memento_debug_cookie(ptr))) {
            memento_debug_fail("foreign-free stack corrupt (block never stamped)");
        }
        *slot = MEMENTO_LIVE_COOKIE; /* local free below re-stamps cleanly */
    }
#else
    #define memento_debug_on_free(ptr)           ((void)0)
    #define memento_debug_on_alloc(ptr)          ((void)0)
    #define memento_debug_mark_free(ptr)         ((void)0)
    #define memento_debug_on_foreign_free(ptr)   ((void)0)
    #define memento_debug_on_foreign_drain(ptr)  ((void)0)
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
        /* getcpu(2) via raw syscall — works even when glibc/musl hide
         * syscall() behind feature-test macros. */
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

/* Forward decls */
static void memento_heap_free_local(memento_thread_heap_t* MEMENTO_RESTRICT heap,
                                    void* MEMENTO_RESTRICT ptr, size_t size);
static void memento_thread_exit_destructor(void* arg);
#if MEMENTO_STATS
static void memento_heap_track_live(memento_thread_heap_t* heap, size_t block_size);
static void memento_heap_track_free(memento_thread_heap_t* heap, size_t block_size);
#endif

static void memento_heap_init(memento_thread_heap_t* heap) {
    memset((void*)heap, 0, sizeof(memento_thread_heap_t));
    for (int i = 0; i < MEMENTO_SIZE_CLASS_COUNT; i++) {
        heap->caches[i].limit = 64;
#if MEMENTO_PAGE_SIZE
        heap->tcache[i].top = 1; /* sentinel in slot[0] */
#endif
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

/* --------------------------------------------------------------------------
 * Heap parking (thread-pool friendly heap recycling)
 * -------------------------------------------------------------------------- */

/* Install `heap` as the calling thread's heap (TLS bookkeeping shared by
 * create and adopt paths). */
static void memento_heap_install_tls(memento_thread_heap_t* heap) {
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

static void memento_heap_clear_tls(memento_thread_heap_t* heap) {
#if MEMENTO_PLATFORM_WINDOWS
    if (memento_fls_ready && memento_fls_index != FLS_OUT_OF_INDEXES) {
        FlsSetValue(memento_fls_index, NULL);
    }
#else
    if (memento_tls_key_created) {
        pthread_setspecific(memento_tls_key, NULL);
    }
#endif
    if (memento_tls_heap == heap) {
        memento_tls_heap = NULL;
    }
}

/* Park `heap` (must be the calling thread's heap, TLS already detached or
 * about to be). Late foreign frees stay safe: they park on the MPSC stack and
 * are drained by the adopting thread or by memento_shutdown. */
static void memento_heap_do_park(memento_thread_heap_t* heap) {
    if (!heap) return;
#if MEMENTO_HEAP_RECYCLING
    memento_thread_heap_flush(heap);
    MEMENTO_REGISTRY_LOCK();
    heap->parked = 1;
    heap->park_next = memento_parked_heaps;
    memento_parked_heaps = heap;
#if MEMENTO_CPU_HEAPS
    {
        unsigned cpu = memento_current_cpu();
        if (memento_cpu_heaps[cpu] == NULL) {
            memento_cpu_heaps[cpu] = heap;
        }
    }
#endif
    MEMENTO_REGISTRY_UNLOCK();
#else
    memento_thread_heap_flush(heap);
    heap->retired = 1;
#endif
}

void memento_heap_park(void) {
    memento_thread_heap_t* heap = memento_tls_heap;
    if (!heap) return;
    memento_heap_clear_tls(heap);
    memento_heap_do_park(heap);
}

memento_thread_heap_t* memento_heap_adopt(void) {
    if (memento_tls_heap) return memento_tls_heap;
#if MEMENTO_HEAP_RECYCLING
    /* NUMA-aware: a parked heap's spans were first-touched by its previous
     * owner, so its memory sits on that thread's node. Adopting across
     * nodes turns every cache miss into a remote one; prefer a heap whose
     * recorded node matches where we are running now. */
    int my_node = -2; /* -2: unknown, matches nothing */
    MEMENTO_REGISTRY_LOCK();
#if MEMENTO_CPU_HEAPS
    {
        unsigned cpu = memento_current_cpu();
        memento_thread_heap_t* ch = memento_cpu_heaps[cpu];
        if (ch && ch->parked) {
            memento_thread_heap_t** pp = &memento_parked_heaps;
            while (*pp) {
                if (*pp == ch) { *pp = ch->park_next; break; }
                pp = &(*pp)->park_next;
            }
            ch->park_next = NULL;
            ch->parked = 0;
            ch->retired = 0;
            memento_cpu_heaps[cpu] = NULL;
            MEMENTO_REGISTRY_UNLOCK();
            ch->thread_id = memento_get_thread_id();
            ch->numa_node = memento_query_numa_node();
            memento_thread_heap_flush(ch);
            memento_heap_install_tls(ch);
            return ch;
        }
    }
#endif
    if (memento_parked_heaps && memento_parked_heaps->park_next) {
        my_node = memento_query_numa_node();
    }
    memento_thread_heap_t* heap = NULL;
    if (my_node >= 0) {
        memento_thread_heap_t** pp = &memento_parked_heaps;
        while (*pp) {
            if ((*pp)->numa_node == my_node) {
                heap = *pp;
                *pp = heap->park_next;
                break;
            }
            pp = &(*pp)->park_next;
        }
    }
    if (!heap) {
        heap = memento_parked_heaps;
        if (heap) memento_parked_heaps = heap->park_next;
    }
    if (heap) {
        heap->park_next = NULL;
        heap->parked = 0;
        heap->retired = 0;
    }
    MEMENTO_REGISTRY_UNLOCK();
    if (heap) {
        heap->thread_id = memento_get_thread_id();
        heap->numa_node = memento_query_numa_node();
        memento_thread_heap_flush(heap); /* drain whatever arrived while parked */
        memento_heap_install_tls(heap);
        return heap;
    }
#endif
    return NULL;
}

memento_thread_heap_t* memento_thread_heap_get(void) {
    if (MEMENTO_UNLIKELY(memento_tls_heap == NULL)) {
        /* Self-initialise: the TLS destructor and atfork handlers are armed
         * here, so embedders can skip memento_init() and still get correct
         * teardown. */
        if (MEMENTO_UNLIKELY(!memento_init())) {
            return NULL;
        }
        /* Adopt a parked heap before paying for a new one. */
        if (memento_heap_adopt()) {
            return memento_tls_heap;
        }
        /* Cache-line aligned so per-class pads land on true line boundaries */
        memento_thread_heap_t* heap = (memento_thread_heap_t*)MEMENTO_ALIGNED_ALLOC(
            MEMENTO_CACHE_LINE_SIZE, sizeof(memento_thread_heap_t));
        if (!heap) return NULL;
        memento_heap_init(heap);
        memento_heap_register(heap);
        memento_heap_install_tls(heap);
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
 * Foreign Deallocation - Intrusive MPSC Stack
 *
 * Push is a CAS loop; drain (memento_thread_heap_flush) is a single atomic
 * EXCHANGE that takes the whole chain. That asymmetry is what makes this
 * ABA-safe without a tagged pointer:
 *
 * The classic Treiber ABA corrupts the *pop* side: pop reads head=A and
 * next=A->next, gets preempted, the stack changes, and the CAS(head, A,
 * A->next) succeeds against a recycled A whose next is stale. Our drain
 * never does that — exchange(head, NULL) removes every node at once, so no
 * node is ever unlinked by a CAS that could observe a stale neighbor.
 *
 * The push side is immune by construction: pusher F reads head=A, writes
 * X->next=A, then CAS(head, A, X). X belongs to F until the CAS lands, so X
 * cannot be recycled mid-flight. And the CAS only succeeds when head==A —
 * in which case X->next=A is exactly the correct link, no matter how often
 * A left the stack and came back. head==A can only mean "A is the current
 * top of stack", and splicing X on top of the current top is always right.
 * (If you ever change the drain to a per-node CAS pop, you MUST add a tag
 * first. You have been warned.)
 * ============================================================================ */

static void memento_span_purge_stale(memento_thread_heap_t* heap, uint64_t now);
static void memento_span_reclaim(memento_thread_heap_t* heap, memento_span_t* span, size_t sc);
static uint64_t memento_now_ms(void);
#if MEMENTO_PAGE_SIZE
static void memento_page_drain(memento_page_t* pg);
static memento_page_t* memento_span_page_at(memento_span_t* span, unsigned i);
#endif

static void memento_foreign_push(memento_thread_heap_t* heap, void* ptr, size_t size) {
    memento_foreign_node_t* node = (memento_foreign_node_t*)ptr;
    memento_debug_on_foreign_free(ptr); /* stamps tag; screams on double-free */
    node->size = size;
    void* head = memento_atomic_ptr_load_relaxed(&heap->foreign_head);
    do {
        node->next = (memento_foreign_node_t*)head;
    } while (!memento_atomic_ptr_cas_release(&heap->foreign_head, head, (void*)node));
}

/* Drain foreign frees, grouped by size class so the subsequent local frees
 * hit one cache line per class. Grouping is a bucket-prepend per class —
 * O(n) pointer swaps, no comparison sort, no item array shuffling. */
void memento_thread_heap_flush(memento_thread_heap_t* heap) {
    if (MEMENTO_UNLIKELY(heap == NULL)) return;
    memento_cached_now_ms = memento_now_ms();
    if (heap->span_free) {
        memento_span_purge_stale(heap, memento_cached_now_ms);
    }
#if MEMENTO_PAGE_SIZE
    for (int i = 0; i < MEMENTO_SIZE_CLASS_COUNT; i++) {
        memento_tcache_bin_t* bin = &heap->tcache[i];
        while (bin->top > 1) {
            void* p = bin->slot[--bin->top];
            memento_page_t* home = memento_page_of(p);
            MEMENTO_ASAN_UNPOISON(p, sizeof(void*));
            *(void**)p = home->free;
            home->free = p;
            if (home->nused) home->nused--;
            {
                size_t bs = memento_size_class_to_size(home->sc);
                int idx = memento_page_idx_of(home, p, bs);
                if (idx >= 0 && (unsigned)idx < home->nbits) {
                    memento_bmp_set(home->bmp, (unsigned)idx);
                }
            }
        }
        memento_page_drain(heap->active[i]);
    }
    for (memento_span_t* span = heap->span_list; span; span = span->next) {
        if (span->reclaimed) continue;
        if (span->sc >= 0 && (size_t)span->sc < MEMENTO_SIZE_CLASS_COUNT &&
            heap->span_partial[span->sc] == span) {
            continue;
        }
        int busy = 0;
        unsigned pi;
        for (pi = 1; pi < MEMENTO_PAGES_PER_SPAN; pi++) {
            if (memento_span_page_at(span, pi)->nused) { busy = 1; break; }
        }
        if (!busy && span->sc >= 0) {
            memento_span_reclaim(heap, span, (size_t)span->sc);
        }
    }
#endif
#if MEMENTO_PLATFORM_POSIX && defined(__linux__)
#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif
    {
        memento_span_t* s;
        for (s = heap->span_list; s; s = s->next) {
            if (s->reclaimed) continue;
            (void)madvise(s, MEMENTO_SPAN_SIZE, MADV_COLLAPSE);
        }
    }
#endif
    if (MEMENTO_LIKELY(memento_atomic_ptr_load_relaxed(&heap->foreign_head) == NULL)) {
        return;
    }

    memento_foreign_node_t* node =
        (memento_foreign_node_t*)memento_atomic_ptr_exchange_acq(&heap->foreign_head, NULL);

    while (node) {
        /* One batch: bucket heads per size class + a large tail chain. */
        memento_foreign_node_t* buckets[MEMENTO_SIZE_CLASS_COUNT];
        memento_foreign_node_t* large = NULL;
        memset(buckets, 0, sizeof(buckets));

        int n = 0;
        while (node && n < MEMENTO_FLUSH_BATCH) {
            memento_foreign_node_t* next = node->next;
            size_t size = node->size;
            memento_debug_on_foreign_drain(node); /* verifies tag, un-stamps */
            if (size <= MEMENTO_MAX_SIZE_CLASS) {
                size_t sc = memento_size_class_for(size);
                node->next = buckets[sc];
                buckets[sc] = node;
            } else {
                node->next = large;
                large = node;
            }
            node = next;
            n++;
        }

        for (size_t sc = 0; sc < MEMENTO_SIZE_CLASS_COUNT; sc++) {
            while (buckets[sc]) {
                memento_foreign_node_t* b = buckets[sc];
                buckets[sc] = b->next;
                memento_heap_free_local(heap, b, b->size);
#if MEMENTO_STATS
                heap->stats.foreign_free_count++;
#endif
            }
        }
        while (large) {
            memento_foreign_node_t* b = large;
            large = b->next;
            memento_heap_free_local(heap, b, b->size);
#if MEMENTO_STATS
            heap->stats.foreign_free_count++;
#endif
        }
    }
}

/* ============================================================================
 * OS mapping helpers (mmap / VirtualAlloc) + THP
 * ============================================================================ */

/* Tell the kernel a range can be discarded (RSS returns; the mapping stays
 * and re-faults zeroed). MADV_* and POSIX_MADV_* both hide behind
 * feature-test macros under strict -std=c11; on Linux the syscall number
 * and the MADV_DONTNEED value (4) are frozen kernel UAPI, so call through
 * the syscall layer as the final fallback. */
#if MEMENTO_PLATFORM_POSIX && defined(__linux__) && defined(SYS_madvise)
extern long syscall(long number, ...);
#endif
static void memento_os_discard(void* ptr, size_t len) {
    (void)ptr;
    (void)len;
#if MEMENTO_PLATFORM_POSIX
    #if defined(MADV_DONTNEED)
    (void)madvise(ptr, len, MADV_DONTNEED);
    #elif defined(POSIX_MADV_DONTNEED)
    (void)posix_madvise(ptr, len, POSIX_MADV_DONTNEED);
    #elif defined(__linux__) && defined(SYS_madvise)
    (void)syscall(SYS_madvise, ptr, len, 4 /* MADV_DONTNEED */);
    #endif
#endif
}

/* Monotonic milliseconds for the purge clock. vDSO on Linux (no syscall),
 * GetTickCount64 on Windows; resolution is more than enough for a ~10 ms
 * policy knob. */
static uint64_t memento_now_ms(void) {
#if MEMENTO_PLATFORM_POSIX
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#else
    return (uint64_t)GetTickCount64();
#endif
}

/* Hit-path reclaim stamps purge_at from this, never from a syscall.
 * flush() refreshes it. */
MEMENTO_FORCE_INLINE uint64_t memento_cached_now(void) {
    return memento_cached_now_ms;
}

/* Mark a range lazily freeable: the kernel may reclaim it under pressure,
 * contents become unspecified. MADV_FREE is Linux 4.5+ (value 8, frozen
 * UAPI); elsewhere this is a no-op and the pages stay committed. */
static void memento_os_lazy_free(void* ptr, size_t len) {
    (void)ptr;
    (void)len;
#if MEMENTO_PLATFORM_POSIX
    #if defined(MADV_FREE)
    (void)madvise(ptr, len, MADV_FREE);
    #elif defined(__linux__) && defined(SYS_madvise)
    (void)syscall(SYS_madvise, ptr, len, 8 /* MADV_FREE */);
    #endif
#endif
}

static void memento_os_advise_thp(void* ptr, size_t mapped) {
    (void)ptr;
    (void)mapped;
#if MEMENTO_PLATFORM_POSIX && MEMENTO_ENABLE_THP
    /* Only on untouched, span-sized-or-larger mappings: THP pays off when
     * the kernel can still collapse pages, and never once the pages were
     * dirtied at 4K granularity. No WILLNEED, no first-touch: prefaulting
     * 2 MiB to hand out 2 KB turns lazy commit into 512 up-front page
     * faults, and the allocating thread touches what it uses soon enough
     * (which is also the correct NUMA policy — first touch by the user). */
    if (mapped >= MEMENTO_SPAN_SIZE) {
    #if defined(MADV_HUGEPAGE)
        (void)madvise(ptr, mapped, MADV_HUGEPAGE);
    #elif defined(__linux__) && defined(SYS_madvise)
        (void)syscall(SYS_madvise, ptr, mapped, 14 /* MADV_HUGEPAGE */);
    #endif
    }
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
    return ptr;
}

static char*  memento_va_base = NULL;
static size_t memento_va_off  = 0;
static size_t memento_va_cap  = 0;
static int    memento_va_ready = 0;

static int memento_va_owns(const void* ptr, size_t size) {
    if (memento_va_ready != 1 || !memento_va_base || !ptr) return 0;
    {
        const char* p = (const char*)ptr;
        const char* lo = memento_va_base;
        const char* hi = memento_va_base + memento_va_cap;
        return p >= lo && p + size <= hi;
    }
}

static void memento_os_free(void* ptr, size_t size) {
    if (MEMENTO_UNLIKELY(!ptr)) return;
    size_t mapped = memento_align_up(size, 4096);
    if (MEMENTO_UNLIKELY(mapped == 0)) mapped = 4096;
    if (memento_va_owns(ptr, mapped)) {
#if MEMENTO_PLATFORM_WINDOWS
        VirtualFree(ptr, mapped, MEM_DECOMMIT);
#elif MEMENTO_PLATFORM_POSIX
        (void)mmap(ptr, mapped, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
#endif
        return;
    }
    MEMENTO_MUNMAP(ptr, mapped);
}

/* Process-wide reserved VA: bump 64 KiB / 2 MiB out of one reservation so
 * we never overmap-and-trim and never race VirtualAlloc on Windows. */
#if MEMENTO_PLATFORM_POSIX
static pthread_mutex_t memento_va_lock = PTHREAD_MUTEX_INITIALIZER;
#define MEMENTO_VA_LOCK()   pthread_mutex_lock(&memento_va_lock)
#define MEMENTO_VA_UNLOCK() pthread_mutex_unlock(&memento_va_lock)
#elif MEMENTO_PLATFORM_WINDOWS
static CRITICAL_SECTION memento_va_lock;
static int memento_va_lock_ready = 0;
#define MEMENTO_VA_LOCK()   EnterCriticalSection(&memento_va_lock)
#define MEMENTO_VA_UNLOCK() LeaveCriticalSection(&memento_va_lock)
#else
#define MEMENTO_VA_LOCK()   ((void)0)
#define MEMENTO_VA_UNLOCK() ((void)0)
#endif

static void memento_va_init(void) {
    if (memento_va_ready) return;
#if MEMENTO_PLATFORM_WINDOWS
    if (!memento_va_lock_ready) {
        InitializeCriticalSection(&memento_va_lock);
        memento_va_lock_ready = 1;
    }
#endif
    memento_va_cap = (size_t)MEMENTO_VA_RESERVE;
#if MEMENTO_PLATFORM_POSIX
    #ifndef MAP_NORESERVE
        #define MAP_NORESERVE 0
    #endif
    void* p = mmap(NULL, memento_va_cap, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { memento_va_ready = -1; return; }
    memento_va_base = (char*)p;
#else
    void* p = VirtualAlloc(NULL, memento_va_cap, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) { memento_va_ready = -1; return; }
    memento_va_base = (char*)p;
#endif
    {
        uintptr_t a = (uintptr_t)memento_va_base;
        uintptr_t aligned = memento_align_up(a, MEMENTO_SPAN_SIZE);
        memento_va_off = (size_t)(aligned - a);
    }
    memento_va_ready = 1;
}

static void* memento_va_bump(size_t size, size_t align) {
    if (memento_va_ready == 0) memento_va_init();
    if (memento_va_ready != 1 || !size) return NULL;
    if (align < 4096) align = 4096;
    MEMENTO_VA_LOCK();
    {
        size_t off = memento_align_up(memento_va_off, align);
        if (off + size > memento_va_cap) {
            MEMENTO_VA_UNLOCK();
            return NULL;
        }
        char* p = memento_va_base + off;
        memento_va_off = off + size;
        MEMENTO_VA_UNLOCK();
#if MEMENTO_PLATFORM_POSIX
        void* r = mmap(p, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (r != (void*)p) return NULL;
#else
        if (!VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE)) return NULL;
#endif
        return p;
    }
}

/* Two-level pagemap: dir = addr>>27 (128 MiB), leaf = addr>>16 (64 KiB).
 * Alloc never consults it. Free / shim use it to reject foreign pointers. */
#define MEMENTO_PM_BUCKETS    128u
#define MEMENTO_PM_DIR_SHIFT  27
#define MEMENTO_PM_LEAF_SHIFT 16
#define MEMENTO_PM_LEAF_LEN   (1u << (MEMENTO_PM_DIR_SHIFT - MEMENTO_PM_LEAF_SHIFT))

#if MEMENTO_PAGE_SIZE
typedef struct memento_pm_dir_s {
    uintptr_t key;
    memento_page_t* leaf[MEMENTO_PM_LEAF_LEN];
    struct memento_pm_dir_s* next;
} memento_pm_dir_t;

static memento_pm_dir_t* memento_pm_bucket[MEMENTO_PM_BUCKETS];

static memento_pm_dir_t* memento_pm_dir(uintptr_t addr, int create) {
    uintptr_t key = addr >> MEMENTO_PM_DIR_SHIFT;
    unsigned b = (unsigned)(key % MEMENTO_PM_BUCKETS);
    memento_pm_dir_t* d = memento_pm_bucket[b];
    while (d) {
        if (d->key == key) return d;
        d = d->next;
    }
    if (!create) return NULL;
    d = (memento_pm_dir_t*)MEMENTO_MALLOC(sizeof(*d));
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    d->key = key;
    d->next = memento_pm_bucket[b];
    memento_pm_bucket[b] = d;
    return d;
}

static void memento_pagemap_set(memento_page_t* pg) {
#if MEMENTO_PAGE_SIZE
    if (!pg) return;
    uintptr_t a = (uintptr_t)pg;
    memento_pm_dir_t* d = memento_pm_dir(a, 1);
    if (d) {
        d->leaf[(a >> MEMENTO_PM_LEAF_SHIFT) & (MEMENTO_PM_LEAF_LEN - 1)] = pg;
    }
#else
    (void)pg;
#endif
}

static void memento_pagemap_clear(memento_page_t* pg) {
#if MEMENTO_PAGE_SIZE
    if (!pg) return;
    uintptr_t a = (uintptr_t)pg;
    memento_pm_dir_t* d = memento_pm_dir(a, 0);
    if (d) {
        d->leaf[(a >> MEMENTO_PM_LEAF_SHIFT) & (MEMENTO_PM_LEAF_LEN - 1)] = NULL;
    }
#else
    (void)pg;
#endif
}

MEMENTO_FORCE_INLINE memento_page_t* memento_pagemap_lookup(const void* ptr) {
    uintptr_t a = (uintptr_t)ptr;
    memento_pm_dir_t* d = memento_pm_dir(a, 0);
    if (!d) return NULL;
    return d->leaf[(a >> MEMENTO_PM_LEAF_SHIFT) & (MEMENTO_PM_LEAF_LEN - 1)];
}
#endif /* MEMENTO_PAGE_SIZE */

/* Global cache of empty spans (not user objects — mutex is fine). */
#if MEMENTO_PAGE_SIZE
static memento_span_t* memento_gspan_head = NULL;
static uint32_t memento_gspan_count = 0;
#if MEMENTO_PLATFORM_POSIX
static pthread_mutex_t memento_gpage_lock = PTHREAD_MUTEX_INITIALIZER;
#define MEMENTO_GPAGE_LOCK()   pthread_mutex_lock(&memento_gpage_lock)
#define MEMENTO_GPAGE_UNLOCK() pthread_mutex_unlock(&memento_gpage_lock)
#elif MEMENTO_PLATFORM_WINDOWS
static CRITICAL_SECTION memento_gpage_lock;
static int memento_gpage_lock_ready = 0;
#define MEMENTO_GPAGE_LOCK()   do { \
    if (!memento_gpage_lock_ready) { \
        InitializeCriticalSection(&memento_gpage_lock); \
        memento_gpage_lock_ready = 1; \
    } \
    EnterCriticalSection(&memento_gpage_lock); \
} while (0)
#define MEMENTO_GPAGE_UNLOCK() LeaveCriticalSection(&memento_gpage_lock)
#else
#define MEMENTO_GPAGE_LOCK()   ((void)0)
#define MEMENTO_GPAGE_UNLOCK() ((void)0)
#endif

static memento_span_t* memento_gspan_pop(void) {
    memento_span_t* s;
    MEMENTO_GPAGE_LOCK();
    s = memento_gspan_head;
    if (s) {
        memento_gspan_head = s->free_next;
        s->free_next = NULL;
        memento_gspan_count--;
    }
    MEMENTO_GPAGE_UNLOCK();
    return s;
}

static int memento_gspan_push(memento_span_t* s) {
    if (!s) return 0;
    MEMENTO_GPAGE_LOCK();
    if (memento_gspan_count >= MEMENTO_GLOBAL_PAGE_CACHE) {
        MEMENTO_GPAGE_UNLOCK();
        return 0;
    }
    s->free_next = memento_gspan_head;
    memento_gspan_head = s;
    memento_gspan_count++;
    MEMENTO_GPAGE_UNLOCK();
    return 1;
}
#endif

#if MEMENTO_CPU_HEAPS
static unsigned memento_current_cpu(void) {
#if MEMENTO_PLATFORM_WINDOWS
    return (unsigned)GetCurrentProcessorNumber() % MEMENTO_CPU_MAX;
#elif defined(__linux__) && defined(SYS_getcpu)
    {
        unsigned cpu = 0, node = 0;
        long rc = syscall(SYS_getcpu, &cpu, &node, (void*)0);
        if (rc == 0) return cpu % MEMENTO_CPU_MAX;
    }
    return 0;
#else
    return 0;
#endif
}
#endif

/* Aligned mapping: used for spans, which must sit at an address aligned to
 * their own size so memento_span_of() is a single AND. Overmap and trim. */
static void* memento_os_alloc_aligned(size_t size, size_t align) {
    {
        void* bumped = memento_va_bump(size, align);
        if (bumped) {
            memento_os_advise_thp(bumped, size);
            return bumped;
        }
    }
#if MEMENTO_PLATFORM_POSIX
    if (align < 4096) align = 4096;
    if (size > SIZE_MAX - align) return NULL;
    size_t total = size + align;
    char* base = (char*)MEMENTO_MMAP(total);
    if (MEMENTO_UNLIKELY(base == MAP_FAILED)) return NULL;
    uintptr_t aligned = memento_align_up((uintptr_t)base, align);
    size_t head = (size_t)(aligned - (uintptr_t)base);
    size_t tail = total - head - size;
    if (head) MEMENTO_MUNMAP(base, head);
    if (tail) MEMENTO_MUNMAP((void*)(aligned + size), tail);
    memento_os_advise_thp((void*)aligned, size);
    return (void*)aligned;
#else
    /* Windows cannot trim a mapping: MEM_RELEASE frees an ENTIRE reservation,
     * so the POSIX map-big/unmap-ends trick does not exist here. What we do
     * instead is reserve big, release, and re-reserve the aligned subrange —
     * a genuine race window (another thread could claim the range between
     * our free and our re-reserve), so we retry, with a fresh ASLR probe each
     * time. 32 attempts at an address-space lottery we keep re-entering;
     * failure returns NULL and the caller propagates OOM, which is the honest
     * outcome. The race-free alternative (keep the oversized reservation and
     * MEM_COMMIT the aligned subrange) pins 2 MiB of address space per span
     * that MEM_RELEASE can never split back out — worse than the race. */
    for (int attempt = 0; attempt < 32; attempt++) {
        char* probe = (char*)VirtualAlloc(NULL, size + align, MEM_RESERVE, PAGE_READWRITE);
        if (!probe) return NULL;
        uintptr_t aligned = memento_align_up((uintptr_t)probe, align);
        VirtualFree(probe, 0, MEM_RELEASE);
        void* p = VirtualAlloc((LPVOID)aligned, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if ((uintptr_t)p == aligned) {
            return p;
        }
        if (p) VirtualFree(p, 0, MEM_RELEASE);
    }
    return NULL;
#endif
}

/* ============================================================================
 * 2 MiB span bump allocator (size-class refill)
 * ============================================================================ */

static memento_span_t* memento_span_create(memento_thread_heap_t* owner) {
    void* mem = memento_os_alloc_aligned(MEMENTO_SPAN_SIZE, MEMENTO_SPAN_SIZE);
    if (MEMENTO_UNLIKELY(!mem)) return NULL;
    memento_span_t* span = (memento_span_t*)mem;
    span->next = NULL;
    span->free_next = NULL;
    span->owner = owner;
    span->capacity = MEMENTO_SPAN_SIZE - MEMENTO_SPAN_HEADER;
    span->carved = 0;
    span->live = 0;
    span->sc = -1;
    span->reclaimed = 0;
    span->purge_at = 0;
    span->zero_from = 0; /* fresh mmap: the whole data region is zero */
    /* Align first block to 16 bytes (header may leave data misaligned) */
    {
        uintptr_t data = (uintptr_t)span->data;
        uintptr_t aligned = (data + 15u) & ~(uintptr_t)15u;
        span->used = (size_t)(aligned - data);
    }
    owner->stats.span_count++;
    return span;
}

/* The initial bump offset within a span (data region, 16-aligned). */
MEMENTO_FORCE_INLINE size_t memento_span_initial_used(const memento_span_t* span) {
    uintptr_t data = (uintptr_t)span->data;
    uintptr_t aligned = (data + 15u) & ~(uintptr_t)15u;
    return (size_t)(aligned - data);
}

/* Fetch the bump span for a class, assigning (or reclaiming) one as needed. */
static memento_span_t* memento_span_for_class(memento_thread_heap_t* heap, size_t sc) {
    memento_span_t* span = heap->span_partial[sc];
    if (MEMENTO_LIKELY(span != NULL)) return span;

    if (heap->span_free) {
        /* Reuse a parked span. zero_from was set at reclaim time: discarded
         * spans (reclaimed == 2) re-fault kernel-zeroed past the first page,
         * retained spans guarantee nothing below their old high-water mark. */
        span = heap->span_free;
        heap->span_free = span->free_next;
        heap->span_free_count--;
        span->free_next = NULL;
        span->reclaimed = 0;
#if MEMENTO_PAGE_SIZE
    } else if ((span = memento_gspan_pop()) != NULL) {
        span->owner = heap;
        span->reclaimed = 0;
        span->next = heap->span_list;
        heap->span_list = span;
        heap->stats.span_count++;
#endif
    } else {
        span = memento_span_create(heap);
        if (!span) return NULL;
        span->next = heap->span_list;
        heap->span_list = span;
    }
    span->sc = (int32_t)sc;
    span->used = memento_span_initial_used(span);
    span->carved = 0;
    span->live = 0;
    heap->span_partial[sc] = span;
    return span;
}

/* A span is empty when no carved block is in user hands (live == 0). Every
 * block then sits on the (single, owner-thread) class freelist — blocks in
 * foreign-flight still count as live, so this is exact. Unlink them and park
 * the span for reuse. The mapping always stays: memento_span_of() must keep
 * resolving stale-looking pointers for the foreign-free path, and the header
 * page is 4 KiB well spent.
 *
 * Pages are not discarded on the spot — that is the madvise ping-pong:
 * the next refill would re-fault 512 pages at ~1 us each, and a mixed
 * workload pays it on every cycle. Instead the span is parked with a purge
 * deadline (MEMENTO_SPAN_PURGE_MS), and each reclaim discards whatever
 * expired (or sits deeper than MEMENTO_SPAN_CACHE_MAX). Churn recycles a
 * span within the window and never pays a fault; a burst that stays freed
 * gets its RSS back. zero_from tracks the guarantee: the old high-water
 * mark while pages are kept, the first discarded page once purged. */
static void memento_span_purge_stale(memento_thread_heap_t* heap, uint64_t now) {
    uint32_t pos = 0;
    for (memento_span_t* s = heap->span_free; s; s = s->free_next, pos++) {
        if (s->reclaimed != 1) continue;
        if (s->purge_at > now && pos < MEMENTO_SPAN_CACHE_MAX) continue;
#if MEMENTO_PLATFORM_POSIX
        /* Keep the first page (span header) mapped; discard the rest. */
        if (MEMENTO_SPAN_SIZE > 4096) {
            memento_os_discard((char*)s + 4096, MEMENTO_SPAN_SIZE - 4096);
        }
#endif
        s->reclaimed = 2;
        s->zero_from = (MEMENTO_SPAN_SIZE > 4096 && MEMENTO_SPAN_HEADER < 4096)
            ? memento_align_up(4096 - MEMENTO_SPAN_HEADER, 16)
            : 0;
        heap->stats.spans_reclaimed++;
    }
}

static void memento_span_reclaim(memento_thread_heap_t* heap,
                                 memento_span_t* span, size_t sc) {
#if MEMENTO_DEBUG && !MEMENTO_PAGE_SIZE
    if (span->live != 0) memento_debug_fail("span reclaim with live blocks");
#endif
#if MEMENTO_PAGE_SIZE
    if (heap->active[sc] && memento_span_of(heap->active[sc]) == span) {
        heap->active[sc] = NULL;
    }
#endif
    memento_size_class_cache_t* cache = &heap->caches[sc];
    void** pp = &cache->head;
    void* n = cache->head;
    while (n) {
        MEMENTO_ASAN_UNPOISON(n, sizeof(void*));
        void* next = *(void**)n;
        if (memento_span_of(n) == span) {
            *pp = next;
            if (cache->count) cache->count--;
            /* stays poisoned: the span is dead until recycled */
        } else {
            MEMENTO_ASAN_POISON(n, sizeof(void*));
            pp = (void**)n;
        }
        n = next;
    }

    /* (postcondition: the walk above unlinked every block of this span) */
    uint64_t now = memento_cached_now();
    span->zero_from = span->used; /* below the high-water mark is dirty */
    span->purge_at = now + MEMENTO_SPAN_PURGE_MS;
    span->reclaimed = 1;
#if MEMENTO_PAGE_SIZE
    if (heap->span_free_count >= MEMENTO_SPAN_CACHE_MAX && memento_gspan_push(span)) {
        {
            memento_span_t** pp = &heap->span_list;
            while (*pp) {
                if (*pp == span) { *pp = span->next; break; }
                pp = &(*pp)->next;
            }
        }
        span->next = NULL;
        span->owner = NULL;
        if (heap->stats.span_count) heap->stats.span_count--;
        memento_span_purge_stale(heap, now);
        return;
    }
#endif
    span->free_next = heap->span_free;
    heap->span_free = span;
    heap->span_free_count++;
    memento_span_purge_stale(heap, now);
}

#if MEMENTO_PAGE_SIZE

#if MEMENTO_SIMD_AVX512 || MEMENTO_SIMD_AVX2
    #include <immintrin.h>
#endif
#if MEMENTO_SIMD_NEON
    #include <arm_neon.h>
#endif

MEMENTO_FORCE_INLINE void* memento_page_slot(memento_page_t* pg, unsigned idx, size_t bs) {
    return (char*)pg + memento_page_payload_off() + (size_t)idx * bs;
}

MEMENTO_FORCE_INLINE int memento_bmp_first_scalar(const uint64_t* b, int nwords) {
    int i;
    for (i = 0; i < nwords; i++) {
        if (b[i]) return i * 64 + (int)__builtin_ctzll(b[i]);
    }
    return -1;
}

#if MEMENTO_SIMD_AVX2
MEMENTO_FORCE_INLINE int memento_bmp_first_avx2(const uint64_t* b, int nwords) {
    const __m256i z = _mm256_setzero_si256();
    int i;
    for (i = 0; i + 4 <= nwords; i += 4) {
        __m256i v = _mm256_loadu_si256((const __m256i*)(b + i));
        __m256i eq = _mm256_cmpeq_epi64(v, z);
        unsigned m = (unsigned)~_mm256_movemask_epi8(eq);
        if (m) {
            int w = __builtin_ctz(m) >> 3;
            return (i + w) * 64 + (int)__builtin_ctzll(b[i + w]);
        }
    }
    {
        int r = memento_bmp_first_scalar(b + i, nwords - i);
        return r < 0 ? -1 : i * 64 + r;
    }
}
#endif

#if MEMENTO_SIMD_AVX512
MEMENTO_FORCE_INLINE int memento_bmp_first_avx512(const uint64_t* b, int nwords) {
    int i;
    for (i = 0; i + 8 <= nwords; i += 8) {
        __m512i v = _mm512_loadu_si512(b + i);
        __mmask8 nz = _mm512_test_epi64_mask(v, v);
        if (nz) {
            int w = __builtin_ctz((unsigned)nz);
            return (i + w) * 64 + (int)__builtin_ctzll(b[i + w]);
        }
    }
    {
        int r = memento_bmp_first_scalar(b + i, nwords - i);
        return r < 0 ? -1 : i * 64 + r;
    }
}
#endif

#if MEMENTO_SIMD_NEON
MEMENTO_FORCE_INLINE int memento_bmp_first_neon(const uint64_t* b, int nwords) {
    int i;
    for (i = 0; i + 2 <= nwords; i += 2) {
        uint64_t lo = b[i];
        uint64_t hi = b[i + 1];
        if (lo) return i * 64 + (int)__builtin_ctzll(lo);
        if (hi) return i * 64 + 64 + (int)__builtin_ctzll(hi);
    }
    {
        int r = memento_bmp_first_scalar(b + i, nwords - i);
        return r < 0 ? -1 : i * 64 + r;
    }
}
#endif

MEMENTO_FORCE_INLINE int memento_bmp_first(const uint64_t* b, int nwords) {
#if MEMENTO_SIMD_AVX512
    return memento_bmp_first_avx512(b, nwords);
#elif MEMENTO_SIMD_AVX2
    return memento_bmp_first_avx2(b, nwords);
#elif MEMENTO_SIMD_NEON
    return memento_bmp_first_neon(b, nwords);
#else
    return memento_bmp_first_scalar(b, nwords);
#endif
}

static void memento_page_reset(memento_page_t* pg, uint8_t sc) {
    size_t bs = memento_size_class_to_size(sc);
    size_t off = memento_page_payload_off();
    uint32_t nbits = 0;
    unsigned i, full, rem;
    pg->free = NULL;
    pg->nfree = 0;
    pg->nused = 0;
    pg->sc = sc;
    pg->state = 0;
    pg->next_partial = NULL;
#if defined(__cplusplus) || !(defined(_MSC_VER) && !defined(__clang__))
    memento_atomic_ptr_store_relaxed(&pg->thread_free, NULL);
#else
    pg->thread_free = NULL;
#endif
    if (bs && off < MEMENTO_PAGE_SIZE) {
        nbits = (uint32_t)((MEMENTO_PAGE_SIZE - off) / bs);
        if (nbits > MEMENTO_PAGE_BMP_WORDS * 64u) {
            nbits = MEMENTO_PAGE_BMP_WORDS * 64u;
        }
    }
    pg->nbits = (uint16_t)nbits;
    memset(pg->bmp, 0, sizeof(pg->bmp));
    full = nbits / 64u;
    for (i = 0; i < full; i++) {
        pg->bmp[i] = ~(uint64_t)0;
    }
    rem = nbits % 64u;
    if (rem) {
        pg->bmp[full] = ((uint64_t)1 << rem) - 1u;
    }
    memento_pagemap_set(pg);
}

static void memento_page_drain(memento_page_t* pg) {
    if (!pg) return;
    memento_foreign_node_t* n =
        (memento_foreign_node_t*)memento_atomic_ptr_exchange_acq(&pg->thread_free, NULL);
    while (n) {
        memento_foreign_node_t* next = n->next;
        memento_debug_on_foreign_drain(n);
        *(void**)n = pg->free;
        pg->free = n;
        if (pg->nused) pg->nused--;
        {
            size_t bs = memento_size_class_to_size(pg->sc);
            int idx = memento_page_idx_of(pg, n, bs);
            if (idx >= 0 && (unsigned)idx < pg->nbits) {
                memento_bmp_set(pg->bmp, (unsigned)idx);
            }
        }
        n = next;
    }
}

static memento_page_t* memento_span_page_at(memento_span_t* span, unsigned i) {
    return (memento_page_t*)((char*)span + (size_t)i * MEMENTO_PAGE_SIZE);
}

static memento_page_t* memento_page_acquire(memento_thread_heap_t* heap, size_t sc,
                                            size_t block_size) {
    (void)block_size;
    memento_page_t* pg = heap->active[sc];
    if (pg) {
        memento_page_drain(pg);
        if (pg->free || memento_bmp_first(pg->bmp, (int)((pg->nbits + 63u) / 64u)) >= 0) {
            return pg;
        }
    }
    memento_span_t* span = heap->span_partial[sc];
    if (!span) {
        span = memento_span_for_class(heap, sc);
        if (!span) return NULL;
        for (unsigned i = 1; i < MEMENTO_PAGES_PER_SPAN; i++) {
            memento_page_reset(memento_span_page_at(span, i), (uint8_t)sc);
        }
    }
    for (unsigned i = 1; i < MEMENTO_PAGES_PER_SPAN; i++) {
        memento_page_t* cand = memento_span_page_at(span, i);
        memento_page_drain(cand);
        if (cand->free || memento_bmp_first(cand->bmp, (int)((cand->nbits + 63u) / 64u)) >= 0) {
            heap->active[sc] = cand;
            return cand;
        }
    }
    /* Span is fully carved. Park it and take a new one. */
    heap->span_partial[sc] = NULL;
    span = memento_span_for_class(heap, sc);
    if (!span) return NULL;
    for (unsigned i = 1; i < MEMENTO_PAGES_PER_SPAN; i++) {
        memento_page_reset(memento_span_page_at(span, i), (uint8_t)sc);
    }
    heap->active[sc] = memento_span_page_at(span, 1);
    return heap->active[sc];
}
#endif

/* Carve a batch of size-class blocks into the freelist; return one for use.
 * *out_fresh (when given) reports whether the returned block is guaranteed
 * kernel-zero — calloc uses it to skip memset. */
static void* memento_refill_size_class(memento_thread_heap_t* MEMENTO_RESTRICT heap,
                                        size_t sc, bool* out_fresh) {
    size_t block_size = memento_size_class_to_size(sc);
#if MEMENTO_PAGE_SIZE
    memento_page_t* pg = memento_page_acquire(heap, sc, block_size);
    if (MEMENTO_UNLIKELY(!pg)) return NULL;
    if (pg->free) {
        void* ptr = pg->free;
        MEMENTO_ASAN_UNPOISON(ptr, sizeof(void*));
        pg->free = *(void**)ptr;
        pg->nused++;
        MEMENTO_ASAN_UNPOISON(ptr, block_size);
        if (out_fresh) *out_fresh = false;
        return ptr;
    }
    /* Bitmap refill: claim K free bits, link backwards, return one. */
    {
        int nwords = (int)((pg->nbits + 63u) / 64u);
        uint32_t batch = MEMENTO_REFILL_BATCH;
        void* blocks[32];
        uint32_t k = 0;
        if (batch > 32) batch = 32;
        while (k < batch) {
            int bit = memento_bmp_first(pg->bmp, nwords);
            if (bit < 0 || (unsigned)bit >= pg->nbits) break;
            memento_bmp_clear(pg->bmp, (unsigned)bit);
            blocks[k++] = memento_page_slot(pg, (unsigned)bit, block_size);
        }
        if (!k) return NULL;
        /* Walk the page backwards: scalar next-stores beat scatter. */
        {
            uint32_t i;
            void* head = pg->free;
            for (i = k; i-- > 0; ) {
                void* b = blocks[i];
                MEMENTO_ASAN_UNPOISON(b, block_size);
                memento_debug_mark_free(b);
                *(void**)b = head;
                head = b;
                MEMENTO_ASAN_POISON(b, block_size);
            }
            pg->free = head;
        }
        {
            void* ptr = pg->free;
            MEMENTO_ASAN_UNPOISON(ptr, block_size);
            pg->free = *(void**)ptr;
            pg->nused++;
            if (out_fresh) *out_fresh = false;
            return ptr;
        }
    }
#else
    memento_size_class_cache_t* MEMENTO_RESTRICT cache = &heap->caches[sc];

    if (cache->head == NULL) {
        cache->count = 0; /* recover the overestimate on the miss path */
    }

    uint32_t room = (cache->count < cache->limit)
        ? (cache->limit - cache->count)
        : 0;
    uint32_t batch = MEMENTO_REFILL_BATCH;
    if (batch > room + 1) {
        batch = room + 1; /* +1 for the block we return */
    }
    if (batch < 1) batch = 1;

    /* The class's bump span; acquire (or reuse a reclaimed) one when full. */
    memento_span_t* span = heap->span_partial[sc];
    if (MEMENTO_UNLIKELY(!span || span->used + block_size > span->capacity)) {
        if (span && span->live == 0) {
            /* Exhausted and empty. The class freelist is empty too (that is
             * why we are refilling), so no block of this span is linked
             * anywhere — restart the bump in place, no madvise round-trip.
             * Blocks in foreign flight count as live, so this is safe.
             * Everything below the old high-water mark is dirty; the rest
             * of the span has never been touched and stays zero. */
            span->zero_from = span->used;
            span->used = memento_span_initial_used(span);
            span->carved = 0;
        } else {
            if (span) heap->span_partial[sc] = NULL; /* exhausted; stays on span_list */
            span = memento_span_for_class(heap, sc);
            if (MEMENTO_UNLIKELY(!span)) return NULL;
        }
    }

    size_t remain = span->capacity - span->used;
    uint32_t fit = (uint32_t)(remain / block_size);
    if (fit > batch) fit = batch;

    void* first = NULL;
    for (uint32_t i = 0; i < fit; i++) {
        void* block = span->data + span->used;
        span->used += block_size;
        span->carved++;
        /* A re-carved block may still carry ASan poison from its previous
         * life (dropped by the free path's cache limit). Unpoison first. */
        MEMENTO_ASAN_UNPOISON(block, block_size);
        if (!first) {
            first = block;
        } else {
            /* Park extras on the freelist (never allocated → no double-free check) */
            memento_debug_mark_free(block);
            *(void**)block = cache->head;
            cache->head = block;
            cache->count++;
            MEMENTO_ASAN_POISON(block, block_size);
        }
    }
    /* The returned block goes straight to the user: it is live. */
    if (first) {
        span->live++;
        if (out_fresh) {
            /* first was carved at the loop's starting offset */
            *out_fresh = (span->used - (size_t)fit * block_size) >= span->zero_from;
        }
    }
    return first;
#endif
}

/* --------------------------------------------------------------------------
 * Live-bytes accounting (hot path: one add; peak tracked branchlessly-ish)
 * -------------------------------------------------------------------------- */

#if MEMENTO_STATS

static void memento_heap_track_live(memento_thread_heap_t* heap, size_t block_size) {
    heap->stats.bytes_live += block_size;
    heap->stats.bytes_allocated += block_size;
    if (MEMENTO_UNLIKELY(heap->stats.bytes_live > heap->stats.bytes_peak)) {
        heap->stats.bytes_peak = heap->stats.bytes_live;
    }
}

static void memento_heap_track_free(memento_thread_heap_t* heap, size_t block_size) {
    heap->stats.bytes_freed += block_size;
    if (heap->stats.bytes_live >= block_size) {
        heap->stats.bytes_live -= block_size;
    } else {
        heap->stats.bytes_live = 0;
    }
}

    #define MEMENTO_STAT_ALLOC(heap, sc, bs) do { \
        (heap)->stats.alloc_count++; \
        (heap)->class_live[sc]++; \
        memento_heap_track_live((heap), (bs)); } while (0)

    #define MEMENTO_STAT_FREE(heap, sc, bs) do { \
        (heap)->stats.free_count++; \
        if ((heap)->class_live[sc]) (heap)->class_live[sc]--; \
        memento_heap_track_free((heap), (bs)); } while (0)

    #define MEMENTO_STAT_ALLOC_LARGE(heap, bs) do { \
        (heap)->stats.alloc_count++; \
        memento_heap_track_live((heap), (bs)); } while (0)

    #define MEMENTO_STAT_FREE_LARGE(heap, bs) do { \
        (heap)->stats.free_count++; \
        memento_heap_track_free((heap), (bs)); } while (0)

#else
    #define MEMENTO_STAT_ALLOC(heap, sc, bs)       ((void)0)
    #define MEMENTO_STAT_FREE(heap, sc, bs)        ((void)0)
    #define MEMENTO_STAT_ALLOC_LARGE(heap, bs)     ((void)0)
    #define MEMENTO_STAT_FREE_LARGE(heap, bs)      ((void)0)
#endif

/* ============================================================================
 * Page-run tier: (8 KiB, 256 KiB] rounded to whole pages, cached per count
 * ============================================================================
 * The 100-KiB-JSON-buffer case: alloc/free cycles at the same page count hit
 * this cache instead of mmap/munmap, so the process stops donating a fresh
 * VMA to the kernel on every lap. No coalescing across page counts — that is
 * tcmalloc's job, not ours; exact-count reuse covers the realloc-dominated
 * workloads this tier exists for.
 */

MEMENTO_FORCE_INLINE size_t memento_page_run_pages_for(size_t total) {
    return (total + MEMENTO_LARGE_META_SIZE + 4095u) >> 12;
}

static void* memento_page_run_alloc(memento_thread_heap_t* MEMENTO_RESTRICT heap,
                                    size_t size, bool* out_fresh) {
    size_t pages = memento_page_run_pages_for(size);
    if (MEMENTO_UNLIKELY(pages == 0 || pages > MEMENTO_PAGE_RUN_CLASSES)) return NULL;
    uint32_t idx = (uint32_t)(pages - 1);

    memento_page_run_node_t* node = heap->page_runs[idx];
    void* base;
    if (node) {
        heap->page_runs[idx] = node->next;
        heap->page_run_count[idx]--;
        heap->stats.page_run_cached--;
        base = (void*)node;
        if (out_fresh) *out_fresh = false; /* recycled: holds old user data */
    } else {
        base = memento_os_alloc(pages << 12);
        if (MEMENTO_UNLIKELY(!base)) return NULL;
        if (out_fresh) *out_fresh = true; /* fresh mmap: kernel-zeroed */
    }

    memento_large_meta_t* meta = (memento_large_meta_t*)base;
    meta->magic = MEMENTO_LARGE_MAGIC;
    meta->pages = pages;
    meta->user_size = size;
    meta->owner = heap;
    return (char*)base + MEMENTO_LARGE_META_SIZE;
}

static bool memento_page_run_free(memento_thread_heap_t* MEMENTO_RESTRICT heap,
                                  void* ptr, size_t pages) {
    /* `pages` comes from the meta header (the truth), not the caller's
     * size — a wrong-size exact-mode free still unmaps the right length. */
    if (MEMENTO_UNLIKELY(pages == 0 || pages > MEMENTO_PAGE_RUN_CLASSES)) return false;
    uint32_t idx = (uint32_t)(pages - 1);
    void* base = (char*)ptr - MEMENTO_LARGE_META_SIZE;

    if (heap->page_run_count[idx] < MEMENTO_PAGE_RUN_CACHE_PER_CLASS) {
        memento_page_run_node_t* node = (memento_page_run_node_t*)base;
        node->next = heap->page_runs[idx];
        heap->page_runs[idx] = node;
        heap->page_run_count[idx]++;
        heap->stats.page_run_cached++;
        /* Only MADV_FREE past the hot set. A same-size allocation is the
         * overwhelmingly likely next event; freeing the hot entries' pages
         * would buy nothing and cost a re-fault on the next alloc. Page 0
         * is never freed: the meta header and this freelist node live
         * there. A taken run is never treated as fresh — MADV_FREE leaves
         * contents unspecified. */
        if (pages > 1 && heap->page_run_count[idx] > MEMENTO_PAGE_RUN_HOT) {
            memento_os_lazy_free((char*)base + 4096, (pages << 12) - 4096);
        }
        return true;
    }
    memento_os_free(base, pages << 12);
    return true;
}

/* ============================================================================
 * Huge-object cache (> MEMENTO_PAGE_RUN_MAX), keyed by PAGE COUNT
 * ============================================================================
 * Exact byte size is the wrong key — realloc(1,000,000) -> (1,000,001) must
 * hit the same slot. Page count is the granularity the OS actually charges
 * us in, so that is what we match on.
 */

MEMENTO_FORCE_INLINE size_t memento_huge_pages_for(size_t size) {
    return (size + MEMENTO_LARGE_META_SIZE + 4095u) >> 12;
}

MEMENTO_FORCE_INLINE void* memento_large_cache_take(memento_thread_heap_t* MEMENTO_RESTRICT heap,
                                                      size_t pages) {
    uint32_t n = heap->large_count;
    for (uint32_t i = 0; i < n; i++) {
        if (heap->large_cache[i].pages == pages) {
            void* ptr = heap->large_cache[i].ptr;
            /* swap-remove */
            heap->large_cache[i] = heap->large_cache[n - 1];
            heap->large_count = n - 1;
            heap->stats.huge_cached--;
            return ptr;
        }
    }
    return NULL;
}

MEMENTO_FORCE_INLINE bool memento_large_cache_put(memento_thread_heap_t* MEMENTO_RESTRICT heap,
                                                    void* ptr, size_t pages) {
    if (pages > MEMENTO_LARGE_CACHE_MAX_PAGES) return false;
    if (heap->large_count >= MEMENTO_LARGE_CACHE_SLOTS) return false;
    heap->large_cache[heap->large_count].ptr = ptr;
    heap->large_cache[heap->large_count].pages = pages;
    heap->large_count++;
    heap->stats.huge_cached++;
    return true;
}

static void* memento_huge_alloc(memento_thread_heap_t* heap, size_t size,
                                bool* out_fresh) {
    size_t pages = memento_huge_pages_for(size);
    void* cached = memento_large_cache_take(heap, pages);
    if (cached) {
        memento_large_meta_t* meta =
            (memento_large_meta_t*)((char*)cached - MEMENTO_LARGE_META_SIZE);
        meta->magic = MEMENTO_LARGE_MAGIC;
        meta->pages = pages;
        meta->user_size = size;
        meta->owner = heap;
        if (out_fresh) *out_fresh = false;
        return cached;
    }
    if (MEMENTO_UNLIKELY(size > SIZE_MAX - MEMENTO_LARGE_META_SIZE)) {
        return NULL;
    }
    void* raw = memento_os_alloc(pages << 12);
    if (raw) {
        memento_large_meta_t* meta = (memento_large_meta_t*)raw;
        meta->magic = MEMENTO_LARGE_MAGIC;
        meta->pages = pages;
        meta->user_size = size;
        meta->owner = heap;
        if (out_fresh) *out_fresh = true;
        return (char*)raw + MEMENTO_LARGE_META_SIZE;
    }
    return NULL;
}

static void memento_huge_free(memento_thread_heap_t* heap, void* ptr,
                              const memento_large_meta_t* meta) {
    if (memento_large_cache_put(heap, ptr, meta->pages)) {
        return;
    }
    memento_os_free((char*)ptr - MEMENTO_LARGE_META_SIZE, meta->pages << 12);
}

/* ============================================================================
 * Thread Heap Allocation — exact-size core
 * ============================================================================
 * The public memento_thread_heap_* entry points select between this exact
 * core and the sized family depending on MEMENTO_SIZED. Internal callers
 * (pool/arena/slab/stack) always use the exact core.
 */

MEMENTO_FORCE_INLINE uint8_t memento_kind_for_total(size_t total) {
    if (total <= MEMENTO_MAX_SIZE_CLASS) return 0; /* SMALL */
    if (memento_page_run_pages_for(total) <= MEMENTO_PAGE_RUN_CLASSES) return 1; /* PAGE_RUN */
    return 2; /* HUGE */
}

/* Slow path: cache miss or large allocation. Kept out-of-line so the hot
 * path stays tiny and I-cache friendly. *out_fresh (when given) reports a
 * kernel-zeroed block. */
static MEMENTO_NOINLINE void* memento_slow_alloc(memento_thread_heap_t* MEMENTO_RESTRICT heap,
                                                   size_t size, size_t sc, bool* out_fresh) {
    if (MEMENTO_LIKELY(size <= MEMENTO_MAX_SIZE_CLASS)) {
        void* ptr = memento_refill_size_class(heap, sc, out_fresh);
        if (ptr) {
            MEMENTO_STAT_ALLOC(heap, sc, memento_size_class_to_size(sc));
            MEMENTO_VALGRIND_MALLOCLIKE(ptr, size);
        }
        return ptr;
    }

    /* Page-run tier */
    if (memento_page_run_pages_for(size) <= MEMENTO_PAGE_RUN_CLASSES) {
        void* ptr = memento_page_run_alloc(heap, size, out_fresh);
        if (ptr) {
            MEMENTO_STAT_ALLOC_LARGE(heap, size);
            MEMENTO_VALGRIND_MALLOCLIKE(ptr, size);
        }
        return ptr;
    }

    /* Huge: page-count cache, then a fresh mapping */
    void* ptr = memento_huge_alloc(heap, size, out_fresh);
    if (ptr) {
        MEMENTO_STAT_ALLOC_LARGE(heap, size);
        MEMENTO_VALGRIND_MALLOCLIKE(ptr, size);
    }
    return ptr;
}

/* Hot path: single cache-line touch on freelist hit.
 * size == 0 is NOT special: it lands in the smallest class, so every mode
 * (exact, sized, shim) returns a unique, freeable pointer — the glibc
 * behaviour real code already depends on. */
MEMENTO_FORCE_INLINE void* memento_fast_alloc(memento_thread_heap_t* MEMENTO_RESTRICT heap,
                                               size_t size, bool* out_fresh) {
    if (MEMENTO_UNLIKELY(heap == NULL)) {
        return NULL;
    }

    if (MEMENTO_LIKELY(size <= MEMENTO_MAX_SIZE_CLASS)) {
        size_t sc = memento_size_class_for(size);
#if MEMENTO_PAGE_SIZE
        memento_tcache_bin_t* MEMENTO_RESTRICT bin = &heap->tcache[sc];
        if (MEMENTO_LIKELY(bin->top > 1)) {
            void* MEMENTO_RESTRICT ptr = bin->slot[--bin->top];
            size_t block_size = memento_size_class_to_size(sc);
            MEMENTO_ASAN_UNPOISON(ptr, block_size);
            memento_debug_on_alloc(ptr);
            MEMENTO_STAT_ALLOC(heap, sc, block_size);
            MEMENTO_VALGRIND_MALLOCLIKE(ptr, size);
            if (out_fresh) *out_fresh = false;
            return ptr;
        }
        memento_page_t* MEMENTO_RESTRICT pg = heap->active[sc];
        if (MEMENTO_LIKELY(pg != NULL && pg->free != NULL)) {
            /* Miss: pack up to 8 from the page into the tcache, then pop. */
            uint32_t n = 1;
            {
                void* packed[8];
                uint32_t pk = 0;
                size_t bs = memento_size_class_to_size(sc);
                while (pk < 8 && (1u + pk) < memento_tcache_depth() && pg->free) {
                    void* p = pg->free;
                    MEMENTO_ASAN_UNPOISON(p, bs);
                    pg->free = *(void**)p;
                    pg->nused++;
                    {
                        int idx = memento_page_idx_of(pg, p, bs);
                        if (idx >= 0 && (unsigned)idx < pg->nbits) {
                            memento_bmp_clear(pg->bmp, (unsigned)idx);
                        }
                    }
                    packed[pk++] = p;
                }
#if MEMENTO_SIMD_AVX2
                if (pk == 8) {
                    _mm256_storeu_si256((__m256i*)&bin->slot[1],
                        _mm256_loadu_si256((const __m256i*)&packed[0]));
                    _mm256_storeu_si256((__m256i*)&bin->slot[5],
                        _mm256_loadu_si256((const __m256i*)&packed[4]));
                    n = 9;
                } else
#elif MEMENTO_SIMD_AVX512
                if (pk == 8) {
                    _mm512_storeu_si512(&bin->slot[1],
                        _mm512_loadu_si512(&packed[0]));
                    n = 9;
                } else
#elif MEMENTO_SIMD_NEON
                if (pk >= 4) {
                    uint32_t s = 0;
                    for (; s + 2 <= pk; s += 2) {
                        vst1q_u64((uint64_t*)&bin->slot[1 + s],
                                  vld1q_u64((const uint64_t*)&packed[s]));
                    }
                    for (; s < pk; s++) bin->slot[1 + s] = packed[s];
                    n = 1 + pk;
                } else
#endif
                {
                    uint32_t s;
                    for (s = 0; s < pk; s++) bin->slot[1 + s] = packed[s];
                    n = 1 + pk;
                }
            }
            bin->top = n;
            if (MEMENTO_LIKELY(bin->top > 1)) {
                void* ptr = bin->slot[--bin->top];
                size_t block_size = memento_size_class_to_size(sc);
                MEMENTO_ASAN_UNPOISON(ptr, block_size);
                memento_debug_on_alloc(ptr);
                MEMENTO_STAT_ALLOC(heap, sc, block_size);
                MEMENTO_VALGRIND_MALLOCLIKE(ptr, size);
                if (out_fresh) *out_fresh = false;
                return ptr;
            }
        }
#else
        memento_size_class_cache_t* MEMENTO_RESTRICT cache = &heap->caches[sc];
        void* MEMENTO_RESTRICT ptr = cache->head;
        if (MEMENTO_LIKELY(ptr != NULL)) {
            size_t block_size = memento_size_class_to_size(sc);
            MEMENTO_ASAN_UNPOISON(ptr, block_size);
            void* next = *(void**)ptr;
            cache->head = next;
            memento_span_t* span = memento_span_of(ptr);
            span->live++;
            if (next) {
                MEMENTO_PREFETCH(next);
            }
            memento_debug_on_alloc(ptr);
            MEMENTO_STAT_ALLOC(heap, sc, block_size);
            MEMENTO_VALGRIND_MALLOCLIKE(ptr, size);
            if (out_fresh) *out_fresh = false;
            return ptr;
        }
#endif
        return memento_slow_alloc(heap, size, sc, out_fresh);
    }

    return memento_slow_alloc(heap, size, 0, out_fresh);
}

/* Exact-size allocation entry used by every internal consumer. */
MEMENTO_FORCE_INLINE void* memento_alloc_exact(memento_thread_heap_t* heap, size_t size) {
    return memento_fast_alloc(heap, size, NULL);
}

/* Freelist push for size-class blocks (LUT mirrored from alloc).
 *
 * The cache limit is honored on free, not just on refill: a block freed past
 * the limit is dropped — poisoned and left inside its span, reusable only
 * when the span empties and is recycled. Unbounded freelists are how "thread
 * allocates 2 GiB of 64-byte nodes, frees them all" turns into permanent RSS;
 * the span live-counter is the release valve: when no block of a span is in
 * user hands (foreign in-flight blocks count as live until drained), every
 * block is on the freelist or dropped, so the whole span goes back. */
MEMENTO_FORCE_INLINE void memento_free_size_class(memento_thread_heap_t* MEMENTO_RESTRICT heap,
                                                    void* MEMENTO_RESTRICT ptr, size_t size) {
    size_t sc = memento_size_class_for(size);
    size_t block_size = memento_size_class_to_size(sc);

    MEMENTO_VALGRIND_FREELIKE(ptr);
    memento_debug_on_free(ptr);
#if MEMENTO_PAGE_SIZE
    memento_tcache_bin_t* bin = &heap->tcache[sc];
    if (MEMENTO_LIKELY(bin->top < memento_tcache_depth())) {
        bin->slot[bin->top++] = ptr;
        MEMENTO_ASAN_POISON(ptr, block_size);
        MEMENTO_STAT_FREE(heap, sc, block_size);
        return;
    }
    /* Spill 8 back to their home pages, keep the rest, then push. */
    {
        uint32_t i;
        for (i = 0; i < 8 && bin->top > 1; i++) {
            void* p = bin->slot[--bin->top];
            memento_page_t* home = memento_page_of(p);
            MEMENTO_ASAN_UNPOISON(p, sizeof(void*));
            *(void**)p = home->free;
            home->free = p;
            if (home->nused) home->nused--;
            {
                size_t bs = memento_size_class_to_size(home->sc);
                int idx = memento_page_idx_of(home, p, bs);
                if (idx >= 0 && (unsigned)idx < home->nbits) {
                    memento_bmp_set(home->bmp, (unsigned)idx);
                }
            }
        }
        bin->slot[bin->top++] = ptr;
    }
    MEMENTO_ASAN_POISON(ptr, block_size);
    MEMENTO_STAT_FREE(heap, sc, block_size);
    {
    memento_page_t* pg = memento_page_of(ptr);
    if (MEMENTO_UNLIKELY(pg->nused == 0 && heap->active[pg->sc] != pg)) {
        memento_span_t* span = memento_span_of(ptr);
        int busy = 0;
        unsigned i;
        for (i = 1; i < MEMENTO_PAGES_PER_SPAN; i++) {
            memento_page_t* pgi = (memento_page_t*)((char*)span +
                (size_t)i * MEMENTO_PAGE_SIZE);
            if (pgi->nused) { busy = 1; break; }
        }
        if (!busy && span->sc >= 0 &&
            (size_t)span->sc < MEMENTO_SIZE_CLASS_COUNT &&
            heap->span_partial[span->sc] != span) {
            memento_span_reclaim(heap, span, (size_t)span->sc);
        }
    }
    }
#else
    memento_size_class_cache_t* MEMENTO_RESTRICT cache = &heap->caches[sc];
    memento_span_t* span = memento_span_of(ptr);
    span->live--;
    if (MEMENTO_LIKELY(cache->count < cache->limit)) {
        *(void**)ptr = cache->head;
        cache->head = ptr;
        cache->count++;
    }
    MEMENTO_ASAN_POISON(ptr, block_size);
    MEMENTO_STAT_FREE(heap, sc, block_size);
    if (MEMENTO_UNLIKELY(span->live == 0)) {
        int32_t real_sc = span->sc;
        if (real_sc >= 0 && (size_t)real_sc < MEMENTO_SIZE_CLASS_COUNT) {
            if (heap->span_partial[real_sc] != span) {
                memento_span_reclaim(heap, span, (size_t)real_sc);
            }
        }
    }
#endif
}

static void memento_heap_free_local(memento_thread_heap_t* MEMENTO_RESTRICT heap,
                                     void* MEMENTO_RESTRICT ptr, size_t size) {
    if (MEMENTO_LIKELY(size <= MEMENTO_MAX_SIZE_CLASS)) {
        /* Span-backed small block. */
        memento_free_size_class(heap, ptr, size);
        return;
    }

    /* Large tiers: the meta header is the source of truth for the mapping's
     * extent, not the caller's size. An exact-mode free with a wrong size
     * still unmaps (or caches) the right number of pages. */
    memento_large_meta_t* meta =
        (memento_large_meta_t*)((char*)ptr - MEMENTO_LARGE_META_SIZE);
    MEMENTO_VALGRIND_FREELIKE(ptr);
#if MEMENTO_DEBUG
    if (MEMENTO_UNLIKELY(meta->magic != MEMENTO_LARGE_MAGIC || meta->pages == 0)) {
        memento_debug_fail("large free: corrupt or missing meta header");
    }
    /* The caller's size may undershoot meta->pages legitimately (realloc
     * shrink hysteresis keeps the mapping); it may never overshoot it. */
    if (MEMENTO_UNLIKELY(memento_page_run_pages_for(size) > meta->pages)) {
        memento_debug_fail("exact-mode free with wrong size");
    }
#endif
    if (meta->pages <= MEMENTO_PAGE_RUN_CLASSES) {
        memento_page_run_free(heap, ptr, (size_t)meta->pages);
    } else {
        memento_huge_free(heap, ptr, meta);
    }
    MEMENTO_STAT_FREE_LARGE(heap, size);
}

/* Exact-size free on the heap that owns the block (routing already done). */
MEMENTO_FORCE_INLINE void memento_free_exact(memento_thread_heap_t* heap,
                                             void* ptr, size_t size) {
    if (MEMENTO_UNLIKELY(ptr == NULL || heap == NULL)) {
        return;
    }
    if (MEMENTO_UNLIKELY(heap != memento_tls_heap)) {
#if MEMENTO_PAGE_SIZE
        if (size <= MEMENTO_MAX_SIZE_CLASS) {
            memento_page_t* pg = memento_page_of(ptr);
            memento_foreign_node_t* node = (memento_foreign_node_t*)ptr;
            memento_debug_on_foreign_free(ptr);
            node->size = size;
            void* head = memento_atomic_ptr_load_relaxed(&pg->thread_free);
            do {
                node->next = (memento_foreign_node_t*)head;
            } while (!memento_atomic_ptr_cas_release(&pg->thread_free, head, (void*)node));
            return;
        }
#endif
        memento_foreign_push(heap, ptr, size);
        return;
    }
    memento_heap_free_local(heap, ptr, size);
}

/* ============================================================================
 * Sized family — 16-byte header, free() needs nothing but the pointer
 * ============================================================================ */

#define MEMENTO_SIZED_MAGIC 0x4D53u /* "MS" */
#define MEMENTO_SIZED_HEADER_SIZE 16u

/* Magic is address-tied so a coincidental 0x4D53 in an exact-mode payload
 * (or in the 16 bytes before an exact pointer) cannot pass as a header.
 * memento_free is sized-only: exact pointers go through thread_heap_free. */
MEMENTO_FORCE_INLINE uint16_t memento_sized_magic_for(const void* user) {
    return (uint16_t)(MEMENTO_SIZED_MAGIC ^ (uint16_t)((uintptr_t)user >> 3));
}

enum {
    MEMENTO_KIND_SMALL = 0,
    MEMENTO_KIND_PAGE_RUN = 1,
    MEMENTO_KIND_HUGE = 2
};

typedef struct {
    uint64_t size;      /* requested user size */
    uint32_t back_off;  /* bytes from the user pointer back to the raw base */
    uint8_t  align_log2;/* requested alignment (log2), >= 4 */
    uint8_t  kind;      /* MEMENTO_KIND_* */
    uint16_t magic;     /* address-tied; see memento_sized_magic_for */
} memento_sized_hdr_t;

MEMENTO_FORCE_INLINE int memento_sized_hdr_ok(const memento_sized_hdr_t* hdr,
                                              const void* user) {
    if (hdr->magic != memento_sized_magic_for(user)) return 0;
    if (hdr->kind > 2) return 0;
    if (hdr->align_log2 < 4 || hdr->align_log2 > 31) return 0;
    {
        size_t extra = (hdr->align_log2 > 4) ? ((size_t)1u << hdr->align_log2) : 0;
        if (hdr->back_off < MEMENTO_SIZED_HEADER_SIZE) return 0;
        if ((size_t)hdr->back_off > MEMENTO_SIZED_HEADER_SIZE + extra) return 0;
    }
    return 1;
}

/* Reconstruct the exact `total` passed to the exact allocator from a header. */
MEMENTO_FORCE_INLINE size_t memento_sized_total(const memento_sized_hdr_t* hdr) {
    size_t extra = (hdr->align_log2 > 4) ? ((size_t)1 << hdr->align_log2) : 0;
    return (size_t)hdr->size + MEMENTO_SIZED_HEADER_SIZE + extra;
}

void* memento_heap_aligned_alloc(memento_thread_heap_t* heap,
                                 size_t alignment, size_t size) {
    if (MEMENTO_UNLIKELY(heap == NULL)) return NULL;
    if (MEMENTO_UNLIKELY(size == 0)) size = 1;
    if (alignment < 16) alignment = 16;
    if (MEMENTO_UNLIKELY(!memento_is_power_of_two(alignment))) return NULL;

    size_t extra = (alignment > 16) ? alignment : 0;
    if (MEMENTO_UNLIKELY(size > SIZE_MAX - MEMENTO_SIZED_HEADER_SIZE - extra)) {
        return NULL;
    }
    size_t total = size + MEMENTO_SIZED_HEADER_SIZE + extra;

    void* raw = memento_alloc_exact(heap, total);
    if (MEMENTO_UNLIKELY(!raw)) return NULL;

    uintptr_t user = memento_align_up((uintptr_t)raw + MEMENTO_SIZED_HEADER_SIZE,
                                      alignment);
    MEMENTO_ASAN_UNPOISON((void*)(user - MEMENTO_SIZED_HEADER_SIZE),
                          MEMENTO_SIZED_HEADER_SIZE);
    memento_sized_hdr_t* hdr = (memento_sized_hdr_t*)(user - MEMENTO_SIZED_HEADER_SIZE);
    hdr->size = size;
    hdr->back_off = (uint32_t)(user - (uintptr_t)raw);
    hdr->align_log2 = memento_log2_pow2(alignment);
    hdr->kind = memento_kind_for_total(total);
    hdr->magic = memento_sized_magic_for((void*)user);
    MEMENTO_VALGRIND_MALLOCLIKE((void*)user, size);
    return (void*)user;
}

void* memento_heap_malloc(memento_thread_heap_t* heap, size_t size) {
    return memento_heap_aligned_alloc(heap, 16, size);
}

/* Recycled-page zero: NT stores / DC ZVA above 4 KiB so we don't RFO. */
static void memento_zero_nt(void* dst, size_t n) {
    unsigned char* p = (unsigned char*)dst;
    if (n < 4096) {
        memset(p, 0, n);
        return;
    }
#if defined(__aarch64__)
    {
        uint64_t dczid;
        size_t zva;
        __asm__ volatile("mrs %0, dczid_el0" : "=r"(dczid));
        if ((dczid & (1ull << 4)) == 0) {
            zva = (size_t)4u << (unsigned)(dczid & 15u);
            if (zva >= 16 && zva <= 256) {
                while ((uintptr_t)p % zva && n) { *p++ = 0; n--; }
                while (n >= zva) {
                    __asm__ volatile("dc zva, %0" :: "r"(p) : "memory");
                    p += zva;
                    n -= zva;
                }
            }
        }
        if (n) memset(p, 0, n);
        return;
    }
#elif MEMENTO_SIMD_AVX512
    {
        while (((uintptr_t)p & 63u) && n) { *p++ = 0; n--; }
        while (n >= 64) {
            _mm512_stream_si512((__m512i*)p, _mm512_setzero_si512());
            p += 64;
            n -= 64;
        }
        _mm_sfence();
        if (n) memset(p, 0, n);
        return;
    }
#elif MEMENTO_SIMD_AVX2
    {
        while (((uintptr_t)p & 31u) && n) { *p++ = 0; n--; }
        while (n >= 32) {
            _mm256_stream_si256((__m256i*)p, _mm256_setzero_si256());
            p += 32;
            n -= 32;
        }
        _mm_sfence();
        if (n) memset(p, 0, n);
        return;
    }
#else
    memset(p, 0, n);
#endif
}

void* memento_heap_calloc(memento_thread_heap_t* heap, size_t count, size_t size) {
    size_t user_size;
    if (MEMENTO_UNLIKELY(memento_mul_overflow(count, size, &user_size))) return NULL;
    if (MEMENTO_UNLIKELY(heap == NULL)) return NULL;
    if (MEMENTO_UNLIKELY(user_size == 0)) user_size = 1; /* match heap_malloc */

    /* Duplicate heap_malloc's header math so the zeroing decision sees the
     * allocation's freshness: pages fresh from the kernel (or back from a
     * MADV_DONTNEED round-trip) are already zero — memset would just fault
     * them in twice. */
    size_t total = user_size + MEMENTO_SIZED_HEADER_SIZE;
    bool fresh = false;
    void* raw = memento_fast_alloc(heap, total, &fresh);
    if (MEMENTO_UNLIKELY(!raw)) return NULL;

    MEMENTO_ASAN_UNPOISON(raw, MEMENTO_SIZED_HEADER_SIZE);
    memento_sized_hdr_t* hdr = (memento_sized_hdr_t*)raw;
    hdr->size = user_size;
    hdr->back_off = MEMENTO_SIZED_HEADER_SIZE;
    hdr->align_log2 = 4; /* 16 */
    hdr->kind = memento_kind_for_total(total);
    hdr->magic = memento_sized_magic_for((char*)raw + MEMENTO_SIZED_HEADER_SIZE);
    void* user = (char*)raw + MEMENTO_SIZED_HEADER_SIZE;
    if (!fresh) memento_zero_nt(user, user_size);
    MEMENTO_VALGRIND_MALLOCLIKE(user, user_size);
    return user;
}

/* Resolve the owning heap from the pointer and hand the block back — to the
 * local freelist if we own it, to the owner's MPSC stack if we don't. This is
 * what makes memento_free(p) a correct cross-thread free. */
static void memento_sized_free_routed(void* ptr) {
    if (MEMENTO_UNLIKELY(ptr == NULL)) return;
    MEMENTO_ASAN_UNPOISON((char*)ptr - MEMENTO_SIZED_HEADER_SIZE,
                          MEMENTO_SIZED_HEADER_SIZE);
    memento_sized_hdr_t* hdr =
        (memento_sized_hdr_t*)((char*)ptr - MEMENTO_SIZED_HEADER_SIZE);
    if (MEMENTO_UNLIKELY(!memento_sized_hdr_ok(hdr, ptr))) {
        /* Debug: scream. Release: no-op. memento_free is sized-only; exact
         * pointers go through memento_thread_heap_free. A coincidental
         * 0x4D53 in foreign memory must not be trusted as a header. */
#if MEMENTO_DEBUG
        memento_debug_fail("memento_free of a non-sized (or corrupt) pointer");
#endif
        return;
    }

    void* raw = (char*)ptr - hdr->back_off;
    size_t total = memento_sized_total(hdr);
    memento_thread_heap_t* owner;

    if (hdr->kind == MEMENTO_KIND_SMALL) {
#if MEMENTO_PAGE_SIZE
        if (!memento_pagemap_lookup(raw)) {
#if MEMENTO_DEBUG
            memento_debug_fail("memento_free of a pointer not in the pagemap");
#endif
            return;
        }
#endif
        owner = memento_span_of(raw)->owner;
    } else {
        memento_large_meta_t* meta =
            (memento_large_meta_t*)((char*)raw - MEMENTO_LARGE_META_SIZE);
        owner = meta->owner;
    }

    MEMENTO_VALGRIND_FREELIKE(ptr);
    hdr->magic = 0; /* double-free through the sized path now fails the check */

    if (MEMENTO_LIKELY(owner == memento_tls_heap)) {
        memento_heap_free_local(owner, raw, total);
    } else if (owner) {
        memento_foreign_push(owner, raw, total);
    }
    /* owner == NULL: heap already released (shutdown) — block leaks, but the
     * mapping it lived in went back to the OS with the heap anyway. */
}

void memento_heap_mfree(memento_thread_heap_t* heap, void* ptr) {
    (void)heap; /* ownership comes from the pointer, not the argument */
    memento_sized_free_routed(ptr);
}

size_t memento_heap_usable_size(const void* ptr) {
    if (!ptr) return 0;
    const memento_sized_hdr_t* hdr =
        (const memento_sized_hdr_t*)((const char*)ptr - MEMENTO_SIZED_HEADER_SIZE);
    if (MEMENTO_UNLIKELY(!memento_sized_hdr_ok(hdr, ptr))) return 0;
    size_t total = memento_sized_total(hdr);
    size_t capacity;
    if (hdr->kind == MEMENTO_KIND_SMALL) {
        capacity = memento_size_class_to_size(memento_size_class_for(total));
    } else {
        /* Large tiers: the mapping's page count is the truth (realloc
         * shrink hysteresis may leave hdr->size below the mapping). */
        const memento_large_meta_t* meta = (const memento_large_meta_t*)
            ((const char*)ptr - hdr->back_off - MEMENTO_LARGE_META_SIZE);
        capacity = ((size_t)meta->pages << 12) - MEMENTO_LARGE_META_SIZE;
    }
    return (capacity > hdr->back_off) ? (capacity - hdr->back_off) : 0;
}

void* memento_heap_realloc(memento_thread_heap_t* heap, void* ptr, size_t new_size) {
    if (ptr == NULL) {
        return memento_heap_malloc(heap, new_size);
    }
    if (new_size == 0) {
        memento_heap_mfree(heap, ptr);
        return NULL;
    }

    /* In-place rules differ by tier, because the free path's bookkeeping
     * does:
     *  - SMALL: freelists are per-class, so the header may only move within
     *    the same size class (the alloc-time class is what free() computes).
     *  - PAGE_RUN / HUGE: free() reads the mapping's page count from the
     *    meta header, so the header is informational. We keep the mapping
     *    whenever the new request still fits it and doesn't drop below half
     *    of the old one — tcmalloc-style hysteresis; a 200K->100K shrink
     *    keeps its pages instead of munmap + mmap + 100K memcpy. */
    memento_sized_hdr_t* hdr =
        (memento_sized_hdr_t*)((char*)ptr - MEMENTO_SIZED_HEADER_SIZE);
    size_t old_total = memento_sized_total(hdr);
    size_t extra = (hdr->align_log2 > 4) ? ((size_t)1 << hdr->align_log2) : 0;
    if (MEMENTO_UNLIKELY(new_size > SIZE_MAX - MEMENTO_SIZED_HEADER_SIZE - extra)) {
        return NULL;
    }
    size_t new_total = new_size + MEMENTO_SIZED_HEADER_SIZE + extra;
    bool in_place;
    if (hdr->kind == MEMENTO_KIND_SMALL && new_total <= MEMENTO_MAX_SIZE_CLASS) {
        in_place = memento_size_class_for(old_total)
                == memento_size_class_for(new_total);
    } else if (hdr->kind != MEMENTO_KIND_SMALL && new_total > MEMENTO_MAX_SIZE_CLASS) {
        const memento_large_meta_t* meta = (const memento_large_meta_t*)
            ((const char*)ptr - hdr->back_off - MEMENTO_LARGE_META_SIZE);
        size_t need_pages = memento_page_run_pages_for(new_total);
        in_place = need_pages <= meta->pages
                && (new_total >= old_total / 2 || need_pages == meta->pages);
    } else {
        in_place = false; /* tier crossing: small->large or large->small */
    }
    if (in_place) {
        hdr->size = new_size;
        return ptr;
    }

    size_t usable = memento_heap_usable_size(ptr);
    void* new_ptr = memento_heap_malloc(heap, new_size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, usable < new_size ? usable : new_size);
        memento_heap_mfree(heap, ptr);
    }
    return new_ptr;
}

/* Global sized family — the calling thread's heap. */

void* memento_malloc(size_t size) {
    return memento_heap_malloc(memento_thread_heap_get(), size);
}

void* memento_calloc(size_t count, size_t size) {
    return memento_heap_calloc(memento_thread_heap_get(), count, size);
}

void* memento_realloc(void* ptr, size_t new_size) {
    if (ptr) {
        /* Route via the owning heap so a cross-thread realloc stays correct. */
        const memento_sized_hdr_t* hdr =
            (const memento_sized_hdr_t*)((const char*)ptr - MEMENTO_SIZED_HEADER_SIZE);
        if (MEMENTO_UNLIKELY(!memento_sized_hdr_ok(hdr, ptr))) {
            /* Not ours. Debug: scream. Release: return NULL — the realloc
             * contract leaves the original pointer valid, so a libc-foreign
             * block survives untouched and the caller keeps working. */
#if MEMENTO_DEBUG
            memento_debug_fail("memento_realloc of a non-sized (or corrupt) pointer");
#endif
            return NULL;
        }
        memento_thread_heap_t* owner;
        if (hdr->kind == MEMENTO_KIND_SMALL) {
            owner = memento_span_of((const char*)ptr - hdr->back_off)->owner;
        } else {
            owner = ((memento_large_meta_t*)
                     ((const char*)ptr - hdr->back_off - MEMENTO_LARGE_META_SIZE))->owner;
        }
        if (MEMENTO_UNLIKELY(owner != memento_tls_heap)) {
            /* Not ours: allocate here, copy, foreign-free the old block.
             * thread_heap_get() (not the raw TLS slot) because this may be
             * this thread's very first memento call. */
            size_t usable = memento_heap_usable_size(ptr);
            void* new_ptr = memento_heap_malloc(memento_thread_heap_get(), new_size);
            if (new_ptr) {
                memcpy(new_ptr, ptr, usable < new_size ? usable : new_size);
                memento_sized_free_routed(ptr);
            }
            return new_ptr;
        }
        return memento_heap_realloc(owner, ptr, new_size);
    }
    return memento_heap_malloc(memento_thread_heap_get(), new_size);
}

void memento_free(void* ptr) {
    memento_sized_free_routed(ptr);
}

void* memento_aligned_alloc(size_t alignment, size_t size) {
    /* C11: size must be a multiple of alignment. posix_memalign does not. */
    if (alignment == 0 || size % alignment != 0) return NULL;
    return memento_heap_aligned_alloc(memento_thread_heap_get(), alignment, size);
}

int memento_posix_memalign(void** out, size_t alignment, size_t size) {
    if (!out) return 22 /* EINVAL */;
    if (alignment < sizeof(void*) || !memento_is_power_of_two(alignment)) {
        *out = NULL;
        return 22 /* EINVAL */;
    }
    void* ptr = memento_heap_aligned_alloc(memento_thread_heap_get(), alignment, size);
    if (!ptr && size != 0) return 12 /* ENOMEM */;
    *out = ptr;
    return 0;
}

size_t memento_usable_size(const void* ptr) {
    return memento_heap_usable_size(ptr);
}

size_t memento_good_size(size_t size) {
    if (size == 0) return 0;
    size_t total = size + MEMENTO_SIZED_HEADER_SIZE;
    if (total <= MEMENTO_MAX_SIZE_CLASS) {
        return memento_size_class_to_size(memento_size_class_for(total))
               - MEMENTO_SIZED_HEADER_SIZE;
    }
    size_t pages = memento_page_run_pages_for(total);
    if (pages <= MEMENTO_PAGE_RUN_CLASSES) {
        return (pages << 12) - MEMENTO_LARGE_META_SIZE - MEMENTO_SIZED_HEADER_SIZE;
    }
    return size; /* huge: you get what you asked for */
}

/* ============================================================================
 * Public thread-heap API — exact or sized, chosen at compile time
 * ============================================================================ */

#if MEMENTO_SIZED

MEMENTO_ATTR_FLATTEN
void* memento_thread_heap_alloc(memento_thread_heap_t* heap, size_t size) {
    return memento_heap_malloc(heap, size);
}

void memento_thread_heap_free(memento_thread_heap_t* heap, void* ptr, size_t size) {
    (void)size; /* header is authoritative in sized mode */
    memento_heap_mfree(heap, ptr);
}

void* memento_thread_heap_realloc(memento_thread_heap_t* heap, void* ptr,
                                  size_t old_size, size_t new_size) {
    (void)old_size;
    return memento_heap_realloc(heap, ptr, new_size);
}

void* memento_thread_heap_alloc_aligned(memento_thread_heap_t* heap,
                                        size_t size, size_t alignment) {
    return memento_heap_aligned_alloc(heap, alignment, size);
}

void memento_thread_heap_free_aligned(memento_thread_heap_t* heap, void* ptr,
                                      size_t size, size_t alignment) {
    (void)size; (void)alignment;
    memento_heap_mfree(heap, ptr);
}

#else /* MEMENTO_EXACT_SIZE */

MEMENTO_ATTR_FLATTEN
void* memento_thread_heap_alloc(memento_thread_heap_t* heap, size_t size) {
    return memento_alloc_exact(heap, size);
}

void memento_thread_heap_free(memento_thread_heap_t* heap, void* ptr, size_t size) {
    memento_free_exact(heap, ptr, size);
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
    } else if (old_size > MEMENTO_MAX_SIZE_CLASS && new_size > MEMENTO_MAX_SIZE_CLASS) {
        /* Page-run/huge hysteresis: same page count → keep the mapping. */
        if (memento_page_run_pages_for(old_size) == memento_page_run_pages_for(new_size)) {
            memento_large_meta_t* meta =
                (memento_large_meta_t*)((char*)ptr - MEMENTO_LARGE_META_SIZE);
            meta->user_size = new_size;
            return ptr;
        }
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

void* memento_thread_heap_alloc_aligned(memento_thread_heap_t* heap,
                                        size_t size, size_t alignment) {
    if (alignment <= 16) {
        return memento_alloc_exact(heap, size);
    }
    if (MEMENTO_UNLIKELY(!memento_is_power_of_two(alignment))) return NULL;
    if (MEMENTO_UNLIKELY(size > SIZE_MAX - alignment - sizeof(void*))) return NULL;

    size_t total = size + alignment + sizeof(void*);
    void* raw = memento_alloc_exact(heap, total);
    if (MEMENTO_UNLIKELY(!raw)) return NULL;

    uintptr_t user = memento_align_up((uintptr_t)raw + sizeof(void*), alignment);
    *(void**)(user - sizeof(void*)) = raw;
    MEMENTO_VALGRIND_MALLOCLIKE((void*)user, size);
    return (void*)user;
}

/* Read the block base stashed one word before an aligned user pointer.
 * Deliberately out of line: when the read is inlined next to the alloc
 * that produced the pointer, GCC's array-bounds pass tracks the pointer's
 * provenance and diagnoses the back-pointer load as an 8-byte underrun.
 * It is not — the aligned path wrote that word itself. */
MEMENTO_NOINLINE static void* memento_aligned_base_of(const void* ptr) {
    void* raw;
    memcpy(&raw, (const char*)ptr - sizeof(raw), sizeof(raw));
    return raw;
}

void memento_thread_heap_free_aligned(memento_thread_heap_t* heap, void* ptr,
                                      size_t size, size_t alignment) {
    if (!ptr) return;
    if (alignment <= 16) {
        memento_free_exact(heap, ptr, size);
        return;
    }
    void* raw = memento_aligned_base_of(ptr);
    MEMENTO_VALGRIND_FREELIKE(ptr);
    memento_free_exact(heap, raw, size + alignment + sizeof(void*));
}

#endif /* MEMENTO_SIZED */

/* ============================================================================
 * Pool Allocator
 * ============================================================================
 * Fixed-size objects carved from backing blocks. A pool starts with one
 * block of `capacity` objects; when it runs dry the next alloc grows it by
 * doubling (bounded), so pools never fail while the heap has memory.
 *
 * Pools are OWNER-THREAD-ONLY: no locking anywhere on the path. Share one
 * across threads behind your own lock, or give each thread its own. Debug
 * builds enforce the owner thread and catch double-frees with an O(free)
 * freelist walk; release builds trust the caller.
 */

typedef struct memento_pool_chunk_s {
    struct memento_pool_chunk_s* next;
} memento_pool_chunk_t;

typedef struct memento_pool_block_s {
    struct memento_pool_block_s* next;
    size_t capacity;     /* objects carved from this block */
} memento_pool_block_t;

#define MEMENTO_POOL_BLOCK_HDR \
    ((sizeof(memento_pool_block_t) + 15u) & ~(size_t)15u)
#define MEMENTO_POOL_MAX_CHUNK (1u << 20) /* growth cap: objects per block */

struct memento_pool_s {
    memento_pool_chunk_t* free_list;
    size_t object_size;
    size_t chunk_capacity; /* objects in the NEXT growth block (doubles) */
    size_t count;          /* free objects on the list */
    size_t total_blocks;
    memento_thread_heap_t* heap;
    memento_pool_block_t* blocks;
#if MEMENTO_DEBUG
    uint64_t owner;        /* owning thread id — pool is owner-thread-only */
#endif
};

#if MEMENTO_DEBUG
#define MEMENTO_DEBUG_OWNER_CHECK(owner_tid, what)                          \
    do {                                                                    \
        if (MEMENTO_UNLIKELY(memento_get_thread_id() != (owner_tid))) {     \
            memento_debug_fail(what " used from a non-owner thread "        \
                               "(pool/stack/slab are owner-thread-only)");  \
        }                                                                   \
    } while (0)
#else
#define MEMENTO_DEBUG_OWNER_CHECK(owner_tid, what) do { (void)0; } while (0)
#endif

static bool memento_pool_grow(memento_pool_t* pool) {
    size_t cap = pool->chunk_capacity;
    size_t block_size;
    if (memento_mul_overflow(pool->object_size, cap, &block_size)) return false;
    if (block_size > SIZE_MAX - MEMENTO_POOL_BLOCK_HDR) return false;
    block_size += MEMENTO_POOL_BLOCK_HDR;

    char* block = (char*)memento_alloc_exact(pool->heap, block_size);
    if (!block) return false;

    memento_pool_block_t* hdr = (memento_pool_block_t*)block;
    hdr->next = pool->blocks;
    hdr->capacity = cap;
    pool->blocks = hdr;
    pool->total_blocks++;

    char* objs = block + MEMENTO_POOL_BLOCK_HDR;
    for (size_t i = 0; i < cap; i++) {
        memento_pool_chunk_t* chunk =
            (memento_pool_chunk_t*)(objs + i * pool->object_size);
        chunk->next = pool->free_list;
        pool->free_list = chunk;
        pool->count++;
    }
    if (pool->chunk_capacity < MEMENTO_POOL_MAX_CHUNK) {
        pool->chunk_capacity *= 2;
        if (pool->chunk_capacity > MEMENTO_POOL_MAX_CHUNK) {
            pool->chunk_capacity = MEMENTO_POOL_MAX_CHUNK;
        }
    }
    return true;
}

memento_pool_t* memento_pool_create(size_t object_size, size_t capacity,
                                     memento_thread_heap_t* heap) {
    if (capacity == 0) return NULL;
    if (object_size < sizeof(void*)) {
        object_size = sizeof(void*);
    }
    object_size = memento_align_up(object_size, sizeof(void*));

    memento_pool_t* pool = (memento_pool_t*)MEMENTO_MALLOC(sizeof(memento_pool_t));
    if (!pool) return NULL;

    pool->object_size = object_size;
    pool->chunk_capacity = capacity;
    pool->count = 0;
    pool->total_blocks = 0;
    pool->heap = heap ? heap : memento_thread_heap_get();
    pool->free_list = NULL;
    pool->blocks = NULL;
#if MEMENTO_DEBUG
    pool->owner = memento_get_thread_id();
#endif

    if (!pool->heap || !memento_pool_grow(pool)) {
        MEMENTO_FREE(pool, sizeof(memento_pool_t));
        return NULL;
    }
    return pool;
}

void memento_pool_destroy(memento_pool_t* pool) {
    if (!pool) return;
    memento_pool_block_t* block = pool->blocks;
    while (block) {
        memento_pool_block_t* next = block->next;
        size_t block_size;
        if (!memento_mul_overflow(pool->object_size, block->capacity, &block_size)) {
            memento_free_exact(pool->heap, block, block_size + MEMENTO_POOL_BLOCK_HDR);
        }
        block = next;
    }
    MEMENTO_FREE(pool, sizeof(memento_pool_t));
}

void* memento_pool_alloc(memento_pool_t* pool) {
    if (!pool) return NULL;
#if MEMENTO_DEBUG
    MEMENTO_DEBUG_OWNER_CHECK(pool->owner, "pool");
#endif
    if (MEMENTO_UNLIKELY(!pool->free_list)) {
        if (!memento_pool_grow(pool)) return NULL;
    }
    memento_pool_chunk_t* chunk = pool->free_list;
    pool->free_list = chunk->next;
    pool->count--;
    return chunk;
}

void memento_pool_free(memento_pool_t* pool, void* ptr) {
    if (!pool || !ptr) return;
#if MEMENTO_DEBUG
    MEMENTO_DEBUG_OWNER_CHECK(pool->owner, "pool");
    /* O(n) double-free detection, debug builds only. */
    for (memento_pool_chunk_t* c = pool->free_list; c; c = c->next) {
        if (MEMENTO_UNLIKELY((void*)c == ptr)) {
            memento_debug_fail("pool double-free");
        }
    }
#endif
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
    size_t size;      /* usable capacity of data[] */
    size_t map_size;  /* guarded blocks: total OS mapping; 0 = heap-carved */
    char data[1];     /* flexible; actual capacity is in size */
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
    uint8_t guarded;
    uint8_t _pad[7];
};

/* Guarded blocks are standalone OS mappings with a PROT_NONE page at the
 * high-water end; they never come from the size-class spans, because you
 * cannot mprotect one page out of somebody else's span. */
static memento_arena_block_t* memento_arena_block_guarded(size_t capacity) {
    size_t live = memento_align_up(MEMENTO_ARENA_HEADER_SIZE + capacity, 4096);
    if (live > SIZE_MAX - 4096) return NULL;
    size_t mapped = live + 4096; /* trailing guard page */
#if MEMENTO_PLATFORM_POSIX
    char* base = (char*)MEMENTO_MMAP(mapped);
    if (MEMENTO_UNLIKELY(base == MAP_FAILED)) return NULL;
    if (mprotect(base + live, 4096, PROT_NONE) != 0) {
        MEMENTO_MUNMAP(base, mapped);
        return NULL;
    }
    /* No first-touch: the arena bumps forward lazily, and the guard page
     * does not care whether the live region was ever touched. */
#else
    char* base = (char*)VirtualAlloc(NULL, mapped, MEM_RESERVE, PAGE_NOACCESS);
    if (!base) return NULL;
    if (!VirtualAlloc(base, live, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(base, 0, MEM_RELEASE);
        return NULL;
    }
#endif
    memento_arena_block_t* block = (memento_arena_block_t*)base;
    block->next = NULL;
    block->size = capacity;
    block->map_size = mapped;
    return block;
}

static void memento_arena_block_free(memento_arena_t* arena,
                                     memento_arena_block_t* block) {
    if (block->map_size) {
#if MEMENTO_PLATFORM_POSIX
        MEMENTO_MUNMAP((void*)block, block->map_size);
#else
        VirtualFree((void*)block, 0, MEM_RELEASE);
#endif
    } else {
        memento_free_exact(arena->heap, block, MEMENTO_ARENA_HEADER_SIZE + block->size);
    }
}

static memento_arena_block_t* memento_arena_new_block(memento_arena_t* arena,
                                                       size_t capacity) {
    if (arena->guarded) {
        return memento_arena_block_guarded(capacity);
    }
    size_t block_size = MEMENTO_ARENA_HEADER_SIZE + capacity;
    memento_arena_block_t* block =
        (memento_arena_block_t*)memento_alloc_exact(arena->heap, block_size);
    if (!block) return NULL;
    block->next = NULL;
    block->size = capacity;
    block->map_size = 0;
    return block;
}

static memento_arena_t* memento_arena_create_impl(size_t initial_capacity,
                                                  memento_thread_heap_t* heap,
                                                  int guarded) {
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
    arena->guarded = (uint8_t)(guarded != 0);

    arena->current = memento_arena_new_block(arena, initial_capacity);
    if (!arena->current) {
        MEMENTO_FREE(arena, sizeof(memento_arena_t));
        return NULL;
    }

    arena->blocks = arena->current;
    arena->top = arena->current->data;
    return arena;
}

memento_arena_t* memento_arena_create(size_t initial_capacity,
                                       memento_thread_heap_t* heap) {
    /* MEMENTO_ARENA_GUARD (default: MEMENTO_DEBUG) upgrades plain arenas to
     * guarded ones in debug builds. */
    return memento_arena_create_impl(initial_capacity, heap, MEMENTO_ARENA_GUARD);
}

memento_arena_t* memento_arena_create_guarded(size_t initial_capacity,
                                               memento_thread_heap_t* heap) {
    /* Explicitly requested by name: always guarded, every build. */
    return memento_arena_create_impl(initial_capacity, heap, 1);
}

void memento_arena_destroy(memento_arena_t* arena) {
    if (!arena) return;
    memento_arena_block_t* block = arena->blocks;
    while (block) {
        memento_arena_block_t* next = block->next;
        memento_arena_block_free(arena, block);
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
        /* Grow geometrically from the CURRENT block — anchoring to the
         * single allocation's need would mint one same-sized block per
         * allocation and turn a run of N appends into N blocks. */
        size_t new_capacity = arena->capacity;
        if (new_capacity == 0) new_capacity = 256;
        if (new_capacity <= SIZE_MAX / 2) {
            new_capacity *= 2;
        }
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

        memento_arena_block_t* block = memento_arena_new_block(arena, new_capacity);
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

    /* Return every block allocated after the save point to the heap. Blocks
     * are newest-first, so everything ahead of the saved block is post-save.
     * A save/restore-heavy loop (parse → eval → rewind) no longer ratchets
     * memory upward. */
    memento_arena_block_t* block = arena->blocks;
    while (block && block != (memento_arena_block_t*)save->saved_block) {
        memento_arena_block_t* next = block->next;
        memento_arena_block_free(arena, block);
        block = next;
    }
    arena->blocks = (memento_arena_block_t*)save->saved_block;

    arena->current = (memento_arena_block_t*)save->saved_block;
    arena->top = save->saved_top;
    arena->used = save->saved_used;
    arena->capacity = save->saved_capacity;
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
            memento_arena_block_free(arena, block);
        }
        block = next;
    }

    if (!keep) {
        keep = memento_arena_new_block(arena, arena->initial_capacity);
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
#if MEMENTO_DEBUG
    uint64_t owner;        /* owner thread — stacks are owner-thread-only */
    void* last_push;       /* LIFO validation: pop must take the last push */
#endif
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
    stack->buffer = (char*)memento_alloc_exact(stack->heap, capacity);
    if (!stack->buffer) {
        MEMENTO_FREE(stack, sizeof(memento_stack_t));
        return NULL;
    }

    stack->capacity = capacity;
    stack->top = 0;
#if MEMENTO_DEBUG
    stack->owner = memento_get_thread_id();
    stack->last_push = NULL;
#endif
    return stack;
}

void memento_stack_destroy(memento_stack_t* stack) {
    if (!stack) return;
    if (stack->buffer) {
        memento_free_exact(stack->heap, stack->buffer, stack->capacity);
    }
    MEMENTO_FREE(stack, sizeof(memento_stack_t));
}

void* memento_stack_push(memento_stack_t* stack, size_t size, size_t alignment) {
    if (!stack || size == 0) return NULL;
#if MEMENTO_DEBUG
    MEMENTO_DEBUG_OWNER_CHECK(stack->owner, "stack");
#endif

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
#if MEMENTO_DEBUG
    stack->last_push = (void*)addr;
#endif
    return (void*)addr;
}

void memento_stack_pop(memento_stack_t* stack, void* ptr) {
    if (!stack || !ptr) return;
#if MEMENTO_DEBUG
    MEMENTO_DEBUG_OWNER_CHECK(stack->owner, "stack");
    if (stack->last_push && MEMENTO_UNLIKELY(ptr != stack->last_push)) {
        memento_debug_fail("stack pop of a non-top pointer (LIFO violation)");
    }
    stack->last_push = NULL; /* history is one deep; next push re-arms it */
#endif
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
#if MEMENTO_DEBUG
        stack->last_push = NULL; /* wholesale rewind: no single top anymore */
#endif
    }
}

void memento_stack_reset(memento_stack_t* stack) {
    if (stack) {
        stack->top = 0;
#if MEMENTO_DEBUG
        stack->last_push = NULL;
#endif
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
#if MEMENTO_DEBUG
    uint64_t owner; /* slabs are owner-thread-only */
#endif
};

memento_slab_t* memento_slab_create(memento_thread_heap_t* heap) {
    memento_slab_t* slab = (memento_slab_t*)MEMENTO_MALLOC(sizeof(memento_slab_t));
    if (!slab) return NULL;

    slab->heap = heap ? heap : memento_thread_heap_get();
    if (!slab->heap) {
        MEMENTO_FREE(slab, sizeof(memento_slab_t));
        return NULL;
    }
#if MEMENTO_DEBUG
    slab->owner = memento_get_thread_id();
#endif

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
            MEMENTO_ASAN_UNPOISON(ptr, slab->classes[i].block_size);
            memento_free_exact(slab->heap, ptr, slab->classes[i].block_size);
        }
    }
    MEMENTO_FREE(slab, sizeof(memento_slab_t));
}

void* memento_slab_alloc(memento_slab_t* slab, size_t size) {
    if (!slab || size == 0) return NULL;
#if MEMENTO_DEBUG
    MEMENTO_DEBUG_OWNER_CHECK(slab->owner, "slab");
#endif
    if (size > MEMENTO_MAX_SIZE_CLASS) {
        return memento_alloc_exact(slab->heap, size);
    }

    size_t sc = memento_size_class_for(size);
    void* ptr = memento_cache_pop(&slab->classes[sc].cache);
    if (!ptr) {
        ptr = memento_alloc_exact(slab->heap, slab->classes[sc].block_size);
    } else {
        MEMENTO_ASAN_UNPOISON(ptr, slab->classes[sc].block_size);
    }
    return ptr;
}

void memento_slab_free(memento_slab_t* slab, void* ptr, size_t size) {
    if (!slab || !ptr) return;
#if MEMENTO_DEBUG
    MEMENTO_DEBUG_OWNER_CHECK(slab->owner, "slab");
#endif

    if (size > MEMENTO_MAX_SIZE_CLASS) {
        memento_free_exact(slab->heap, ptr, size);
        return;
    }

    size_t sc = memento_size_class_for(size);
#if MEMENTO_DEBUG
    /* Bounded double-free detection: the per-class cache is capped at
     * limit entries, so this walk is O(limit), debug builds only. */
    for (void* n = slab->classes[sc].cache.head; n; n = *(void**)n) {
        if (MEMENTO_UNLIKELY(n == ptr)) {
            memento_debug_fail("slab double-free");
        }
    }
#endif
    if (!memento_cache_push(&slab->classes[sc].cache, ptr)) {
        memento_free_exact(slab->heap, ptr, slab->classes[sc].block_size);
    }
}

/* ============================================================================
 * Proxy layer — one instrumented front-end for every allocator
 * ============================================================================ */

enum {
    MEMENTO_PROXY_KIND_HEAP = 0,
    MEMENTO_PROXY_KIND_POOL = 1,
    MEMENTO_PROXY_KIND_ARENA = 2,
    MEMENTO_PROXY_KIND_STACK = 3,
    MEMENTO_PROXY_KIND_SLAB = 4
};

#define MEMENTO_PROXY_COOKIE_MAGIC 0xC00C1E5AFE5ULL

typedef struct {
    void* ptr;           /* user pointer (table key) */
    void* raw;           /* block base — differs from ptr for aligned allocs */
    size_t total;        /* exact-mode free size for raw (0 in sized mode) */
    const char* file;    /* site, for display */
    uint32_t line;
    uint32_t size;       /* requested bytes, for reports */
#if MEMENTO_PROXY_DEBUG
    uint64_t cookie;
    uint64_t owner;      /* allocating thread id */
#endif
} memento_proxy_entry_t;

/* The tracking table is ALWAYS on, release included: memento_proxy_free()
 * takes no size argument, and in exact mode a free without the allocation's
 * true size is a wrong-class free (heap corruption once spans reclaim). A
 * proxy is something you wrap around an allocator to see what it is doing —
 * one open-addressed hash lookup per call is the price of that answer.
 * MEMENTO_PROXY_DEBUG adds the cookie/owner fields and their verification. */
typedef struct memento_proxy_state_s {
    int kind;
    uint32_t flags;
    void* target;                        /* the wrapped allocator */
    memento_thread_heap_t* owner_heap;   /* heap it draws from (for reports) */
    struct memento_proxy_state_s* heap_next;
    memento_proxy_entry_t* table;        /* open-addressed live-allocation set */
    size_t cap;
    size_t count;
    size_t tombstones;                   /* (void*)1 slots; compacted on grow */
    size_t live_bytes;
    size_t peak_bytes;
    uint64_t total_allocs;
    uint64_t total_frees;
} memento_proxy_state_t;

uint64_t memento_site_id(const char* file, int line) {
    /* FNV-1a over the path, folded with the line number. */
    uint64_t h = 1469598103934665603ull;
    if (file) {
        const unsigned char* p = (const unsigned char*)file;
        while (*p) {
            h ^= *p++;
            h *= 1099511628211ull;
        }
    }
    h ^= (uint64_t)(uint32_t)line;
    h *= 1099511628211ull;
    return h;
}

/* ---- underlying allocator dispatch --------------------------------------
 * Every alloc reports the raw block base and the exact-mode free size, so
 * the free path never has to guess: exact mode frees raw+total, sized mode
 * frees by header off the user pointer. Aligned allocs (align > 16) are the
 * only case where raw != ptr. */
static void* memento_proxy_raw_alloc(memento_proxy_state_t* st,
                                     size_t size, size_t align,
                                     void** out_raw, size_t* out_total) {
    void* ptr = NULL;
    void* raw = NULL;
    size_t total = 0;
    switch (st->kind) {
    case MEMENTO_PROXY_KIND_HEAP: {
        memento_thread_heap_t* heap = (memento_thread_heap_t*)st->target;
#if MEMENTO_SIZED
        ptr = memento_thread_heap_alloc_aligned(heap, size, align ? align : 16);
        raw = ptr;   /* sized free routes by header off the user pointer */
        total = 0;
#else
        size_t alignment = align ? align : 16;
        if (alignment <= 16) {
            ptr = memento_alloc_exact(heap, size);
            raw = ptr;
            total = size;
        } else {
            ptr = memento_thread_heap_alloc_aligned(heap, size, alignment);
            if (ptr) {
                raw = memento_aligned_base_of(ptr);
                total = size + alignment + sizeof(void*);
            }
        }
#endif
        break;
    }
    case MEMENTO_PROXY_KIND_POOL:
        (void)align;
        ptr = memento_pool_alloc((memento_pool_t*)st->target);
        raw = ptr;
        total = 0;
        break;
    case MEMENTO_PROXY_KIND_ARENA:
        ptr = memento_arena_alloc((memento_arena_t*)st->target, size,
                                  align ? align : 1);
        raw = ptr;
        total = 0;
        break;
    case MEMENTO_PROXY_KIND_STACK:
        ptr = memento_stack_push((memento_stack_t*)st->target, size,
                                 align ? align : 1);
        raw = ptr;
        total = 0;
        break;
    case MEMENTO_PROXY_KIND_SLAB:
        (void)align;
        ptr = memento_slab_alloc((memento_slab_t*)st->target, size);
        raw = ptr;
        total = 0;
        break;
    }
    if (ptr) {
        *out_raw = raw;
        *out_total = total;
    }
    return ptr;
}

static void memento_proxy_raw_free(memento_proxy_state_t* st,
                                   void* ptr, void* raw,
                                   size_t total, size_t size) {
    (void)ptr;
    (void)raw;
    (void)total;
    switch (st->kind) {
    case MEMENTO_PROXY_KIND_HEAP:
#if MEMENTO_SIZED
        memento_thread_heap_free((memento_thread_heap_t*)st->target, ptr, 0);
#else
        memento_thread_heap_free((memento_thread_heap_t*)st->target, raw, total);
#endif
        break;
    case MEMENTO_PROXY_KIND_POOL:
        memento_pool_free((memento_pool_t*)st->target, ptr);
        break;
    case MEMENTO_PROXY_KIND_ARENA:
        /* Arenas reclaim wholesale; free only untracks. */
        break;
    case MEMENTO_PROXY_KIND_STACK:
        memento_stack_pop((memento_stack_t*)st->target, ptr);
        break;
    case MEMENTO_PROXY_KIND_SLAB:
        memento_slab_free((memento_slab_t*)st->target, ptr, size);
        break;
    }
}

#if MEMENTO_PROXY_DEBUG
static uint64_t memento_proxy_cookie_for(void* ptr, size_t size, uint64_t site) {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t)(uintptr_t)ptr;      h *= 1099511628211ull;
    h ^= (uint64_t)size;                h *= 1099511628211ull;
    h ^= site;                          h *= 1099511628211ull;
    h ^= MEMENTO_PROXY_COOKIE_MAGIC;
    return h;
}
#endif

static void memento_proxy_table_grow(memento_proxy_state_t* st) {
    size_t new_cap = st->cap ? st->cap * 2 : 64;
    memento_proxy_entry_t* nt = (memento_proxy_entry_t*)MEMENTO_MALLOC(
        new_cap * sizeof(memento_proxy_entry_t));
    if (!nt) return; /* out of memory: keep tracking what we have */
    memset(nt, 0, new_cap * sizeof(memento_proxy_entry_t));
    for (size_t i = 0; i < st->cap; i++) {
        memento_proxy_entry_t e = st->table[i];
        if (e.ptr && e.ptr != (void*)1) {
            size_t mask = new_cap - 1;
            size_t j = (size_t)((uintptr_t)e.ptr >> 4) & mask;
            while (nt[j].ptr && nt[j].ptr != (void*)1) {
                j = (j + 1) & mask;
            }
            nt[j] = e;
        }
    }
    if (st->table) MEMENTO_FREE(st->table, st->cap * sizeof(memento_proxy_entry_t));
    st->table = nt;
    st->cap = new_cap;
    st->tombstones = 0;
}

/* Same-capacity rehash that drops tombstones. 24-hour processes otherwise
 * fill the table with (void*)1 slots and every lookup walks them. */
static void memento_proxy_table_compact(memento_proxy_state_t* st) {
    if (!st->cap || !st->tombstones) return;
    size_t cap = st->cap;
    memento_proxy_entry_t* nt = (memento_proxy_entry_t*)MEMENTO_MALLOC(
        cap * sizeof(memento_proxy_entry_t));
    if (!nt) return;
    memset(nt, 0, cap * sizeof(memento_proxy_entry_t));
    size_t mask = cap - 1;
    for (size_t i = 0; i < cap; i++) {
        memento_proxy_entry_t e = st->table[i];
        if (e.ptr && e.ptr != (void*)1) {
            size_t j = (size_t)((uintptr_t)e.ptr >> 4) & mask;
            while (nt[j].ptr) {
                j = (j + 1) & mask;
            }
            nt[j] = e;
        }
    }
    MEMENTO_FREE(st->table, cap * sizeof(memento_proxy_entry_t));
    st->table = nt;
    st->tombstones = 0;
}

static memento_proxy_entry_t* memento_proxy_table_find(memento_proxy_state_t* st,
                                                       void* ptr) {
    if (!st->cap) return NULL;
    size_t mask = st->cap - 1;
    size_t i = (size_t)((uintptr_t)ptr >> 4) & mask;
    while (st->table[i].ptr) {
        if (st->table[i].ptr == ptr) return &st->table[i];
        i = (i + 1) & mask;
    }
    return NULL;
}

static void memento_proxy_track(memento_proxy_state_t* st, void* ptr,
                                void* raw, size_t total,
                                size_t size, const char* file, int line) {
    if ((st->count + 1) * 10 >= st->cap * 7) {
        memento_proxy_table_grow(st);
        if ((st->count + 1) * 10 >= st->cap * 7) return; /* grow failed */
    }
    if (st->tombstones > 16 && st->tombstones * 2 > st->count) {
        memento_proxy_table_compact(st);
    }
    size_t mask = st->cap - 1;
    size_t i = (size_t)((uintptr_t)ptr >> 4) & mask;
    uint32_t replaced = 0;
    while (st->table[i].ptr && st->table[i].ptr != (void*)1) {
        if (MEMENTO_UNLIKELY(st->table[i].ptr == ptr)) {
#if MEMENTO_PROXY_DEBUG
            memento_debug_fail("proxy: pointer tracked twice (double-alloc?)");
#else
            /* Release: arena resets legitimately re-issue live addresses.
             * Replace the entry in place; don't double-count it. */
            if (st->live_bytes >= st->table[i].size)
                st->live_bytes -= st->table[i].size;
            replaced = 1;
            break;
#endif
        }
        i = (i + 1) & mask;
    }
    if (st->table[i].ptr == (void*)1 && st->tombstones) {
        st->tombstones--;
    }
    st->table[i].ptr = ptr;
    st->table[i].raw = raw;
    st->table[i].total = total;
    st->table[i].file = file;
    st->table[i].line = (uint32_t)line;
    st->table[i].size = (uint32_t)size;
#if MEMENTO_PROXY_DEBUG
    uint64_t site = memento_site_id(file, line);
    st->table[i].cookie = memento_proxy_cookie_for(ptr, size, site);
    st->table[i].owner = memento_get_thread_id();
#endif
    if (!replaced) st->count++;
    st->live_bytes += size;
    if (st->live_bytes > st->peak_bytes) st->peak_bytes = st->live_bytes;
    st->total_allocs++;
}

/* Returns 1 and fills the out-params when ptr is tracked, 0 otherwise. */
static int memento_proxy_untrack(memento_proxy_state_t* st, void* ptr,
                                 void** raw_out, size_t* total_out,
                                 size_t* size_out, const char** file_out,
                                 int* line_out) {
    memento_proxy_entry_t* e = memento_proxy_table_find(st, ptr);
    if (!e) return 0;
#if MEMENTO_PROXY_DEBUG
    if ((st->flags & MEMENTO_PROXY_COOKIE)) {
        uint64_t site = memento_site_id(e->file, (int)e->line);
        if (e->cookie != memento_proxy_cookie_for(ptr, e->size, site)) {
            memento_debug_fail("proxy: cookie mismatch (corrupt or foreign pointer)");
        }
    }
#endif
    *raw_out = e->raw;
    *total_out = e->total;
    *size_out = e->size;
    if (file_out) *file_out = e->file;
    if (line_out) *line_out = (int)e->line;
    {
        size_t nbytes = e->size;
        e->ptr = (void*)1; /* tombstone */
        st->tombstones++;
        st->count--;
        if (st->live_bytes >= nbytes) st->live_bytes -= nbytes;
        else st->live_bytes = 0;
        st->total_frees++;
    }
    if (st->tombstones > 16 && st->tombstones * 2 > st->count) {
        memento_proxy_table_compact(st);
    }
    return 1;
}

void* memento_proxy_alloc_site(memento_proxy_t* proxy, size_t size,
                               size_t align, const char* file, int line) {
    if (!proxy || !proxy->impl) return NULL;
    memento_proxy_state_t* st = (memento_proxy_state_t*)proxy->impl;
    void* raw = NULL;
    size_t total = 0;
    void* ptr = memento_proxy_raw_alloc(st, size, align, &raw, &total);
    if (ptr) {
        memento_proxy_track(st, ptr, raw, total, size, file, line);
#if MEMENTO_PROXY_DEBUG
        if (st->flags & MEMENTO_PROXY_TRACE) {
            fprintf(stderr, "memento proxy %p: alloc %zu @%p (%s:%d)\n",
                    (void*)proxy, size, ptr, file ? file : "?", line);
        }
#endif
    }
    return ptr;
}

void memento_proxy_free(memento_proxy_t* proxy, void* ptr) {
    if (!proxy || !proxy->impl || !ptr) return;
    memento_proxy_state_t* st = (memento_proxy_state_t*)proxy->impl;
    void* raw = NULL;
    size_t total = 0;
    size_t size = 0;
    const char* file = NULL;
    int line = 0;
    if (!memento_proxy_untrack(st, ptr, &raw, &total, &size, &file, &line)) {
#if MEMENTO_PROXY_DEBUG
        memento_debug_fail("proxy: free of untracked pointer "
                           "(double-free or foreign allocation)");
#else
        /* Release builds never abort, but freeing blind would corrupt the
         * exact-mode heap — warn and leak instead. */
        fprintf(stderr, "memento proxy: free of untracked pointer %p "
                "(double-free or foreign allocation) — ignored\n", ptr);
#endif
        return;
    }
#if MEMENTO_PROXY_DEBUG
    if (st->flags & MEMENTO_PROXY_TRACE) {
        fprintf(stderr, "memento proxy %p: free %zu @%p (allocated %s:%d)\n",
                (void*)proxy, size, ptr, file ? file : "?", line);
    }
#endif
    memento_proxy_raw_free(st, ptr, raw, total, size);
}

/* vtable thunks so proxy->alloc(proxy->impl, ...) works without the macro */
static void* memento_proxy_thunk_alloc(void* impl, size_t size, size_t align) {
    memento_proxy_t holder;
    holder.impl = impl;
    holder.flags = 0;
    holder._pad = 0;
    holder.alloc = NULL;
    holder.free_ = NULL;
    return memento_proxy_alloc_site(&holder, size, align, "(thunk)", 0);
}

static void memento_proxy_thunk_free(void* impl, void* ptr, size_t size) {
    (void)size; /* the proxy's own table knows the size, in every build */
    memento_proxy_t holder;
    holder.impl = impl;
    holder.flags = 0;
    holder._pad = 0;
    holder.alloc = NULL;
    holder.free_ = NULL;
    memento_proxy_free(&holder, ptr);
}

static memento_proxy_t* memento_proxy_wrap(void* target, int kind,
                                           memento_thread_heap_t* owner_heap,
                                           uint32_t flags) {
    if (!target) return NULL;
    memento_proxy_t* proxy = (memento_proxy_t*)MEMENTO_MALLOC(sizeof(memento_proxy_t));
    if (!proxy) return NULL;
    memento_proxy_state_t* st =
        (memento_proxy_state_t*)MEMENTO_MALLOC(sizeof(memento_proxy_state_t));
    if (!st) {
        MEMENTO_FREE(proxy, sizeof(memento_proxy_t));
        return NULL;
    }
    memset(st, 0, sizeof(*st));
    st->kind = kind;
    st->flags = flags;
    st->target = target;
    st->owner_heap = owner_heap;

    proxy->alloc = memento_proxy_thunk_alloc;
    proxy->free_ = memento_proxy_thunk_free;
    proxy->impl = st;
    proxy->flags = flags;
    proxy->_pad = 0;

    /* Register on the heap so memento_thread_heap_report can see us. */
    if (owner_heap) {
        st->heap_next = owner_heap->proxy_list;
        owner_heap->proxy_list = st;
    }
    return proxy;
}

memento_proxy_t* memento_proxy_wrap_heap(memento_thread_heap_t* heap, uint32_t flags) {
    return memento_proxy_wrap(heap, MEMENTO_PROXY_KIND_HEAP, heap, flags);
}

memento_proxy_t* memento_proxy_wrap_pool(memento_pool_t* pool, uint32_t flags) {
    return memento_proxy_wrap(pool, MEMENTO_PROXY_KIND_POOL,
                              pool ? pool->heap : NULL, flags);
}

memento_proxy_t* memento_proxy_wrap_arena(memento_arena_t* arena, uint32_t flags) {
    return memento_proxy_wrap(arena, MEMENTO_PROXY_KIND_ARENA,
                              arena ? arena->heap : NULL, flags);
}

memento_proxy_t* memento_proxy_wrap_stack(memento_stack_t* stack, uint32_t flags) {
    return memento_proxy_wrap(stack, MEMENTO_PROXY_KIND_STACK,
                              stack ? stack->heap : NULL, flags);
}

memento_proxy_t* memento_proxy_wrap_slab(memento_slab_t* slab, uint32_t flags) {
    return memento_proxy_wrap(slab, MEMENTO_PROXY_KIND_SLAB,
                              slab ? slab->heap : NULL, flags);
}

void memento_proxy_destroy(memento_proxy_t* proxy) {
    if (!proxy || !proxy->impl) return;
    memento_proxy_state_t* st = (memento_proxy_state_t*)proxy->impl;
    /* Unlink from the owner heap's proxy list. */
    if (st->owner_heap) {
        memento_proxy_state_t** pp = &st->owner_heap->proxy_list;
        while (*pp) {
            if (*pp == st) {
                *pp = st->heap_next;
                break;
            }
            pp = &(*pp)->heap_next;
        }
    }
    if (st->table) {
        MEMENTO_FREE(st->table, st->cap * sizeof(memento_proxy_entry_t));
    }
    MEMENTO_FREE(st, sizeof(memento_proxy_state_t));
    MEMENTO_FREE(proxy, sizeof(memento_proxy_t));
}

size_t memento_proxy_outstanding_count(memento_proxy_t* proxy) {
    if (!proxy || !proxy->impl) return 0;
    return ((memento_proxy_state_t*)proxy->impl)->count;
}

size_t memento_proxy_outstanding_bytes(memento_proxy_t* proxy) {
    if (!proxy || !proxy->impl) return 0;
    return ((memento_proxy_state_t*)proxy->impl)->live_bytes;
}

static const char* memento_proxy_kind_name(int kind) {
    switch (kind) {
    case MEMENTO_PROXY_KIND_HEAP:  return "heap";
    case MEMENTO_PROXY_KIND_POOL:  return "pool";
    case MEMENTO_PROXY_KIND_ARENA: return "arena";
    case MEMENTO_PROXY_KIND_STACK: return "stack";
    case MEMENTO_PROXY_KIND_SLAB:  return "slab";
    }
    return "?";
}

void memento_proxy_report(memento_proxy_t* proxy, FILE* out) {
    if (!out) out = stderr;
    if (!proxy || !proxy->impl) {
        fprintf(out, "memento proxy report: (null proxy)\n");
        return;
    }
    memento_proxy_state_t* st = (memento_proxy_state_t*)proxy->impl;
    fprintf(out, "memento proxy report (%s, flags=0x%x)\n",
            memento_proxy_kind_name(st->kind), st->flags);
    fprintf(out, "  allocs %llu · frees %llu · outstanding %zu blocks, %zu bytes (peak %zu)\n",
            (unsigned long long)st->total_allocs,
            (unsigned long long)st->total_frees,
            st->count, st->live_bytes, st->peak_bytes);

    if (st->count == 0) {
        fprintf(out, "  no outstanding allocations — clean.\n");
        return;
    }

    /* Top 10 sites by outstanding bytes. */
    typedef struct {
        const char* file;
        uint32_t line;
        size_t count;
        size_t bytes;
    } site_agg_t;
    site_agg_t top[10];
    memset(top, 0, sizeof(top));

    for (size_t i = 0; i < st->cap; i++) {
        memento_proxy_entry_t* e = &st->table[i];
        if (!e->ptr || e->ptr == (void*)1) continue;
        int slot = -1;
        for (int j = 0; j < 10; j++) {
            if (top[j].file == e->file && top[j].line == e->line) {
                slot = j;
                break;
            }
            if (slot < 0 && top[j].count == 0 && top[j].bytes == 0) {
                slot = j; /* first empty, but keep looking for a match */
            }
        }
        if (slot >= 0) {
            top[slot].file = e->file;
            top[slot].line = e->line;
            top[slot].count++;
            top[slot].bytes += e->size;
        }
    }
    /* sort by bytes desc */
    for (int i = 1; i < 10; i++) {
        site_agg_t key = top[i];
        int j = i - 1;
        while (j >= 0 && top[j].bytes < key.bytes) {
            top[j + 1] = top[j];
            j--;
        }
        top[j + 1] = key;
    }
    fprintf(out, "  top sites (outstanding):\n");
    int shown = 0;
    for (int i = 0; i < 10; i++) {
        if (!top[i].count) break;
        fprintf(out, "    %s:%u — %zu blocks, %zu bytes\n",
                top[i].file ? top[i].file : "?", top[i].line,
                top[i].count, top[i].bytes);
        shown++;
    }
    if (!shown) {
        fprintf(out, "    (no site data)\n");
    }
    if (st->kind == MEMENTO_PROXY_KIND_ARENA) {
        fprintf(out, "  note: arena proxies reclaim at reset/destroy; \"outstanding\" "
                     "means allocated since the last reset and never proxy-freed.\n");
    }
}

/* ============================================================================
 * Heap report
 * ============================================================================ */

void memento_thread_heap_stats(memento_thread_heap_t* heap, memento_heap_stats_t* stats) {
    if (heap && stats) {
        *stats = heap->stats;
    }
}

int memento_thread_heap_numa_node(const memento_thread_heap_t* heap) {
    if (MEMENTO_UNLIKELY(!heap)) return -1;
    return (int)heap->numa_node;
}

/* Count nodes parked on the foreign stack without taking it (owner thread or
 * quiescent process only — a relaxed walk while producers push is racy). */
static size_t memento_heap_foreign_pending(memento_thread_heap_t* heap) {
    size_t n = 0;
    memento_foreign_node_t* node =
        (memento_foreign_node_t*)memento_atomic_ptr_load_relaxed(&heap->foreign_head);
    while (node) {
        n++;
        node = node->next;
    }
    return n;
}

void memento_thread_heap_report(memento_thread_heap_t* heap, FILE* out) {
    if (!out) out = stderr;
    if (!heap) {
        fprintf(out, "memento heap report: (null heap)\n");
        return;
    }

    const char* state = heap->parked ? "parked"
                      : heap->retired ? "retired" : "active";
    fprintf(out, "memento heap report — tid %llu — %s\n",
            (unsigned long long)heap->thread_id, state);
    fprintf(out,
            "  allocs %zu · frees %zu · foreign frees %zu · foreign pending %zu\n",
            heap->stats.alloc_count, heap->stats.free_count,
            heap->stats.foreign_free_count, memento_heap_foreign_pending(heap));
    fprintf(out,
            "  bytes live %zu (peak %zu) · spans %zu (%zu MiB, %zu reclaimed) · "
            "page runs cached %zu · huge cached %zu\n",
            heap->stats.bytes_live, heap->stats.bytes_peak,
            heap->stats.span_count,
            heap->stats.span_count * (MEMENTO_SPAN_SIZE >> 20),
            heap->stats.spans_reclaimed,
            heap->stats.page_run_cached, heap->stats.huge_cached);

    if (heap->retired || heap->parked) {
        size_t pending = memento_heap_foreign_pending(heap);
        if (pending) {
            fprintf(out, "  this heap is %s, %zu blocks parked on the foreign stack\n",
                    state, pending);
        }
    }

    /* Top size classes by live block count */
    typedef struct { size_t size; uint64_t live; } class_row_t;
    class_row_t rows[MEMENTO_SIZE_CLASS_COUNT];
    int nrows = 0;
#if MEMENTO_STATS
    for (int i = 0; i < MEMENTO_SIZE_CLASS_COUNT; i++) {
        if (heap->class_live[i]) {
            rows[nrows].size = memento_size_class_to_size((size_t)i);
            rows[nrows].live = heap->class_live[i];
            nrows++;
        }
    }
#else
    fprintf(out, "  (per-class live counts compiled out: MEMENTO_STATS=0)\n");
#endif
    for (int i = 1; i < nrows; i++) {
        class_row_t key = rows[i];
        int j = i - 1;
        while (j >= 0 && rows[j].live < key.live) {
            rows[j + 1] = rows[j];
            j--;
        }
        rows[j + 1] = key;
    }
    if (nrows) {
        fprintf(out, "  top size classes (live):\n");
        int shown = nrows < 8 ? nrows : 8;
        for (int i = 0; i < shown; i++) {
            fprintf(out, "    %5zu B × %llu = %llu B\n",
                    rows[i].size, (unsigned long long)rows[i].live,
                    (unsigned long long)(rows[i].size * rows[i].live));
        }
    }

    /* Proxies wrapped around this heap */
    memento_proxy_state_t* st = heap->proxy_list;
    if (st) {
        fprintf(out, "  proxies:\n");
    }
    while (st) {
        fprintf(out, "    [%s] outstanding %zu blocks, %zu bytes (peak %zu)\n",
                memento_proxy_kind_name(st->kind),
                st->count, st->live_bytes, st->peak_bytes);
        st = st->heap_next;
    }
}

void memento_report_all(FILE* out) {
    if (!out) out = stderr;
    MEMENTO_REGISTRY_LOCK();
    memento_thread_heap_t* heap = memento_heap_registry;
    int n = 0;
    while (heap) {
        memento_thread_heap_report(heap, out);
        heap = heap->registry_next;
        n++;
    }
    MEMENTO_REGISTRY_UNLOCK();
    if (!n) {
        fprintf(out, "memento: no heaps registered\n");
    }
}

/* ============================================================================
 * Global Initialization
 * ============================================================================ */

void memento_heap_release_caches(memento_thread_heap_t* heap) {
    /* Size-class freelist nodes live inside spans — drop the lists, then unmap. */
    for (int i = 0; i < MEMENTO_SIZE_CLASS_COUNT; i++) {
        heap->caches[i].head = NULL;
        heap->caches[i].count = 0;
#if MEMENTO_PAGE_SIZE
        heap->tcache[i].top = 1;
        heap->active[i] = NULL;
#endif
    }

    /* Page-run cache → OS */
    for (uint32_t i = 0; i < MEMENTO_PAGE_RUN_CLASSES; i++) {
        memento_page_run_node_t* node = heap->page_runs[i];
        while (node) {
            memento_page_run_node_t* next = node->next;
            memento_os_free((void*)node, (size_t)(i + 1) << 12);
            node = next;
        }
        heap->page_runs[i] = NULL;
        heap->page_run_count[i] = 0;
    }
    heap->stats.page_run_cached = 0;

    /* Huge-object cache → OS */
    for (uint32_t i = 0; i < heap->large_count; i++) {
        void* ptr = heap->large_cache[i].ptr;
        size_t pages = heap->large_cache[i].pages;
        if (ptr) {
            memento_os_free((char*)ptr - MEMENTO_LARGE_META_SIZE,
                            pages << 12);
        }
    }
    heap->large_count = 0;
    heap->stats.huge_cached = 0;

    /* Unmap every span (span_free spans are on span_list too) */
    memento_span_t* span = heap->span_list;
    heap->span_list = NULL;
    heap->span_free = NULL;
    heap->span_free_count = 0;
    for (int i = 0; i < MEMENTO_SIZE_CLASS_COUNT; i++) {
        heap->span_partial[i] = NULL;
    }
    while (span) {
        memento_span_t* next = span->next;
#if MEMENTO_PAGE_SIZE
        {
            unsigned pi;
            for (pi = 1; pi < MEMENTO_PAGES_PER_SPAN; pi++) {
                memento_pagemap_clear(memento_span_page_at(span, pi));
            }
        }
#endif
        memento_os_free(span, MEMENTO_SPAN_SIZE);
        span = next;
    }
    heap->stats.span_count = 0;
}

void memento_malloc_trim(void) {
#if MEMENTO_PAGE_SIZE
    memento_span_t* s;
    while ((s = memento_gspan_pop()) != NULL) {
        {
            unsigned pi;
            for (pi = 1; pi < MEMENTO_PAGES_PER_SPAN; pi++) {
                memento_pagemap_clear(memento_span_page_at(s, pi));
            }
        }
        memento_os_free(s, MEMENTO_SPAN_SIZE);
    }
#endif
    MEMENTO_REGISTRY_LOCK();
    {
        memento_thread_heap_t* heap = memento_heap_registry;
        while (heap) {
            memento_span_purge_stale(heap, ~(uint64_t)0);
            heap = heap->registry_next;
        }
    }
    MEMENTO_REGISTRY_UNLOCK();
}

memento_mallinfo_t memento_mallinfo(void) {
    memento_mallinfo_t info;
    memset(&info, 0, sizeof(info));
    MEMENTO_REGISTRY_LOCK();
    {
        memento_thread_heap_t* heap = memento_heap_registry;
        while (heap) {
            info.uordblks += heap->stats.bytes_live;
            info.arena += heap->stats.span_count * (size_t)MEMENTO_SPAN_SIZE;
            info.ordblks += heap->span_free_count;
            info.hblks += heap->large_count;
            info.keepcost += heap->span_free_count * (size_t)MEMENTO_SPAN_SIZE;
            heap = heap->registry_next;
        }
    }
    MEMENTO_REGISTRY_UNLOCK();
#if MEMENTO_PAGE_SIZE
    info.ordblks += memento_gspan_count;
    info.keepcost += memento_gspan_count * (size_t)MEMENTO_SPAN_SIZE;
#endif
    info.fordblks = info.keepcost;
    info.hblkhd = info.hblks * (size_t)MEMENTO_PAGE_RUN_MAX;
    return info;
}

int memento_ctl(int op, void* arg) {
    switch (op) {
    case MEMENTO_CTL_GET_TCACHE_DEPTH:
        if (!arg) return -1;
        *(size_t*)arg = memento_tcache_depth();
        return 0;
    case MEMENTO_CTL_SET_TCACHE_DEPTH:
        if (!arg) return -1;
        {
            size_t v = *(size_t*)arg;
            if (v < 2 || v > MEMENTO_TCACHE_DEPTH_MAX) return -1;
            memento_rt_tcache_depth = (uint32_t)v;
            return 0;
        }
    case MEMENTO_CTL_TRIM:
        memento_malloc_trim();
        return 0;
    default:
        return -1;
    }
}

/* pthread/FLS TLS destructor: runs on thread exit (not for the main thread).
 * Drain foreign frees that arrived while we were still "owner" and park the
 * heap (or retire it, with recycling disabled). Do NOT release spans/caches
 * here: live allocations may still be held by other threads and foreign-freed
 * later (they write into the block itself for the MPSC node). Spans stay
 * mapped until memento_shutdown. */
static void memento_thread_exit_destructor(void* arg) {
    memento_thread_heap_t* heap = (memento_thread_heap_t*)arg;
    if (!heap) return;
    memento_heap_do_park(heap);
    memento_tls_heap = NULL;
}

#if MEMENTO_PLATFORM_POSIX
/* Bumped in the fork child. Embedders that cache heap pointers (a wrapper
 * struct, a memoized memento_thread_heap_get()) can compare generations to
 * detect that their cached heap is now retired state. Plain storage: it is
 * only ever written in the single-threaded fork child, read via __atomic. */
static volatile unsigned long memento_fork_generation = 0;

unsigned long memento_fork_generation_current(void) {
    return __atomic_load_n(&memento_fork_generation, __ATOMIC_RELAXED);
}

static void memento_atfork_prepare(void) {
    MEMENTO_REGISTRY_LOCK();
}

static void memento_atfork_parent(void) {
    MEMENTO_REGISTRY_UNLOCK();
}

static void memento_atfork_child(void) {
    /* The child is single-threaded now. Re-init the lock (the parent may have
     * held it), and retire EVERY heap — including the forking thread's own.
     * Its spans are CoW copies of the parent's: allocating on them would
     * duplicate every page the parent keeps dirtying, and the child would
     * bump-carve into pages whose contents it shares. The child's next
     * allocation creates a fresh heap with fresh, private spans instead.
     *
     * Freeing pre-fork pointers in the child stays correct: ownership is
     * resolved through the span header, which routes to the (now retired)
     * original heap's foreign stack, drained at memento_shutdown. */
    pthread_mutex_t fresh = PTHREAD_MUTEX_INITIALIZER;
    memento_registry_lock = fresh;
    memento_parked_heaps = NULL;
    memento_thread_heap_t* heap = memento_heap_registry;
    while (heap) {
        heap->retired = 1;
        heap->parked = 0;
        heap->park_next = NULL;
        heap = heap->registry_next;
    }
    memento_tls_heap = NULL;
    if (memento_tls_key_created) {
        pthread_setspecific(memento_tls_key, NULL); /* disarm the destructor */
    }
    __atomic_add_fetch(&memento_fork_generation, 1, __ATOMIC_RELAXED);
}

int memento_atfork_register(void) {
    if (memento_atfork_registered) return 0;
    int rc = pthread_atfork(memento_atfork_prepare,
                            memento_atfork_parent,
                            memento_atfork_child);
    if (rc == 0) memento_atfork_registered = 1;
    return rc;
}
#else
unsigned long memento_fork_generation_current(void) {
    return 0; /* no fork() on this platform */
}
int memento_atfork_register(void) {
    return 0; /* no fork() on this platform */
}
#endif

/* One-time global init: TLS destructor key/FLS slot, registry lock
 * (Windows), atfork handlers (POSIX). Exactly-once per process, safe to
 * race — pthread_once / InitOnceExecuteOnce, not a check-then-set flag.
 * memento_thread_heap_get() calls this, so the library self-initializes on
 * first use; calling memento_init() explicitly is still fine (and lets you
 * surface the error). */
static void memento_apply_env(void) {
    const char* e = getenv("MEMENTO_TCACHE_DEPTH");
    if (e && e[0]) {
        unsigned long v = strtoul(e, NULL, 10);
        if (v >= 2 && v <= (unsigned long)MEMENTO_TCACHE_DEPTH_MAX) {
            memento_rt_tcache_depth = (uint32_t)v;
        }
    }
}

#if MEMENTO_PLATFORM_WINDOWS
static BOOL CALLBACK memento_init_once_cb(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&memento_registry_lock);
    memento_fls_index = FlsAlloc(memento_thread_exit_destructor);
    if (memento_fls_index == FLS_OUT_OF_INDEXES) {
        return FALSE;
    }
    memento_fls_ready = 1;
    memento_apply_env();
    memento_va_init();
    memento_initialized = 1;
    return TRUE;
}
#else
static void memento_init_once_body(void) {
    if (pthread_key_create(&memento_tls_key, memento_thread_exit_destructor) == 0) {
        memento_tls_key_created = 1;
        memento_atfork_register();
        memento_apply_env();
        memento_va_init();
        memento_initialized = 1;
    }
}
#endif

bool memento_init(void) {
#if MEMENTO_PLATFORM_WINDOWS
    InitOnceExecuteOnce(&memento_init_once, memento_init_once_cb, NULL, NULL);
#else
    pthread_once(&memento_init_once, memento_init_once_body);
#endif
    return memento_initialized != 0;
}

void memento_shutdown(void) {
    if (!memento_initialized) return;

    MEMENTO_REGISTRY_LOCK();
    memento_thread_heap_t* heap = memento_heap_registry;
    memento_heap_registry = NULL;
    memento_parked_heaps = NULL;
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
        memento_heap_clear_tls(heap);
        MEMENTO_ALIGNED_FREE(heap);
        heap = next;
    }

    /* The once-guarded resources (TLS key / FLS slot, registry lock, atfork
     * handlers) stay armed: they cost nothing idle, pthread_once cannot
     * re-run, and a thread that touches memento after shutdown must still
     * get a working heap with a working exit destructor. Shutdown means
     * "release every heap's memory now", not "uninitialise the process". */
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
