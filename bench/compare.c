/*
 * compare.c — head-to-head allocator comparison harness.
 *
 * One allocator per binary, selected with -DALLOC_*. Identical workloads,
 * run sequentially on the same machine, same minute; take the min of three
 * runs and compare orderings, not absolutes.
 *
 *   -DALLOC_MIMENTO3      current tree's include/memento.h (default -I path)
 *   -DALLOC_MIMENTO221    v2.2.1 header (point -I at a v2.2.1 checkout)
 *   -DALLOC_MIMALLOC      link libmimalloc
 *   -DALLOC_RPMALLOC      link rpmalloc
 *   (none)                system malloc
 *
 * Every run takes an optional arg: any argument = quick mode (iters / 4).
 *
 * Hand-built examples (what the README table used):
 *   cc -O2 -DALLOC_MIMENTO3  -Iinclude        bench/compare.c -o bc_v3  -lpthread
 *   cc -O2 -DALLOC_MIMALLOC  -I$MI/include    bench/compare.c $MI/libmimalloc.a -o bc_mi -lpthread
 *   cc -O2 -DALLOC_RPMALLOC  -I$RP/rpmalloc   bench/compare.c $RP/librpmalloc_lib.a -o bc_rp -lpthread
 *   cc -O2                                  bench/compare.c -o bc_glibc -lpthread
 *
 * The CMake build wires up compare_{memento,mimalloc,rpmalloc,glibc}
 * automatically (FetchContent supplies mimalloc/rpmalloc).
 *
 * Methodology notes, hard-earned:
 *  - The churn loops carry a compiler barrier (`asm volatile`) around the
 *    pointer: without it GCC proves the malloc/free pair dead and glibc
 *    "measures" 0.00 ns/op.
 *  - al_setup_thread stores the memento heap in THREAD-LOCAL storage. An
 *    earlier version kept it in a global and let four threads race on one
 *    heap — memento heaps are single-owner by design, and the resulting
 *    freelist corruption hung the box. Don't do that.
 *  - memento 2.2.1 prefaults its spans (MADV_WILLNEED + first-touch at
 *    creation), so its bulk numbers exclude fault costs the current tree
 *    pays lazily inside the timed region. See the README before panicking
 *    about the bulk-64B row.
 */

#if !defined(_WIN32)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

#if defined(ALLOC_MIMENTO3)
  #define MEMENTO_IMPLEMENTATION
  #include "memento.h"
#elif defined(ALLOC_MIMENTO221)
  #define MEMENTO_IMPLEMENTATION
  #include "memento.h"   /* -I points at the v2.2.1 checkout */
#elif defined(ALLOC_MIMALLOC)
  #include "mimalloc.h"
#elif defined(ALLOC_RPMALLOC)
  #include "rpmalloc.h"
#endif

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ---- uniform adapter ---- */
#if defined(ALLOC_MIMENTO3) || defined(ALLOC_MIMENTO221)
static _Thread_local memento_thread_heap_t* g_heap; /* per-thread: heaps are single-owner */
#endif

static void al_setup_thread(void) {
#if defined(ALLOC_MIMENTO3) || defined(ALLOC_MIMENTO221)
    g_heap = memento_thread_heap_get();
#elif defined(ALLOC_RPMALLOC)
    rpmalloc_thread_initialize();
#endif
}
static void al_teardown_thread(void) {
#if defined(ALLOC_RPMALLOC)
    rpmalloc_thread_finalize(1);
#endif
}
static void* al_alloc(size_t n) {
#if defined(ALLOC_MIMENTO3) || defined(ALLOC_MIMENTO221)
    return memento_thread_heap_alloc(g_heap, n);
#elif defined(ALLOC_MIMALLOC)
    return mi_malloc(n);
#elif defined(ALLOC_RPMALLOC)
    return rpmalloc(n);
#else
    return malloc(n);
#endif
}
static void al_free(void* p, size_t n) {
#if defined(ALLOC_MIMENTO3) || defined(ALLOC_MIMENTO221)
    memento_thread_heap_free(g_heap, p, n);
#elif defined(ALLOC_MIMALLOC)
    mi_free(p);
#elif defined(ALLOC_RPMALLOC)
    rpfree(p);
#else
    free(p);
#endif
}

static void report(const char* name, double t0, double t1, long ops) {
    printf("  %-26s %7.2f ns/op\n", name, (t1 - t0) * 1e9 / (double)ops);
}

/* hot alloc+free loop */
static void bench_churn(const char* name, size_t size, long iters) {
    for (long i = 0; i < 1000; i++) al_free(al_alloc(size), size);
    double t0 = now_sec();
    for (long i = 0; i < iters; i++) {
        void* p = al_alloc(size);
        asm volatile("" : : "r"(p) : "memory");
        al_free(p, size);
    }
    report(name, t0, now_sec(), iters);
}

/* allocate everything, then free everything in reverse */
static void bench_bulk(const char* name, size_t size, long n) {
    void** ptrs = (void**)malloc((size_t)n * sizeof(void*));
    for (long i = 0; i < 1000; i++) al_free(al_alloc(size), size);
    double t0 = now_sec();
    for (long i = 0; i < n; i++) ptrs[i] = al_alloc(size);
    for (long i = n - 1; i >= 0; i--) al_free(ptrs[i], size);
    report(name, t0, now_sec(), n);
    free(ptrs);
}

/* mixed sizes 16..256, bulk pattern */
static void bench_mixed(const char* name, long n) {
    void** ptrs = (void**)malloc((size_t)n * sizeof(void*));
    size_t* sz = (size_t*)malloc((size_t)n * sizeof(size_t));
    unsigned rng = 42;
    for (long i = 0; i < n; i++) {
        rng = rng * 1664525u + 1013904223u;
        sz[i] = 16 + (rng % 241);
    }
    double t0 = now_sec();
    for (long i = 0; i < n; i++) ptrs[i] = al_alloc(sz[i]);
    for (long i = n - 1; i >= 0; i--) al_free(ptrs[i], sz[i]);
    report(name, t0, now_sec(), n);
    free(sz);
    free(ptrs);
}

/* N threads, each churning independently */
static void* thread_churn(void* arg) {
    long iters = (long)(intptr_t)arg;
    al_setup_thread();
    for (long i = 0; i < 1000; i++) al_free(al_alloc(64), 64);
    for (long i = 0; i < iters; i++) {
        void* p = al_alloc(64);
        asm volatile("" : : "r"(p) : "memory");
        al_free(p, 64);
    }
    al_teardown_thread();
    return NULL;
}

static void bench_threads(const char* name, int nthreads, long iters_per) {
    pthread_t t[16];
    double t0 = now_sec();
    for (int i = 0; i < nthreads; i++)
        pthread_create(&t[i], NULL, thread_churn, (void*)(intptr_t)iters_per);
    for (int i = 0; i < nthreads; i++)
        pthread_join(t[i], NULL);
    report(name, t0, now_sec(), (long)nthreads * iters_per);
}

int main(int argc, char** argv) {
    const char* who = "glibc";
#if defined(ALLOC_MIMENTO3)
#if defined(MEMENTO_STATS) && !MEMENTO_STATS
    who = "memento-3.0-nostats";
#else
    who = "memento-3.0";
#endif
    memento_init();
#elif defined(ALLOC_MIMENTO221)
    who = "memento-2.2.1"; memento_init();
#elif defined(ALLOC_MIMALLOC)
    who = "mimalloc-2.1.7";
#elif defined(ALLOC_RPMALLOC)
    who = "rpmalloc-1.4.5"; rpmalloc_initialize();
#endif
    int quick = (argc > 1); /* quick: shorter loops for warm-up sanity */
    long k = quick ? 4 : 1;
    al_setup_thread();

    printf("[%s]\n", who);
    bench_churn("churn-64B", 64, 2000000 / k);
    bench_churn("churn-1KiB", 1024, 1000000 / k);
    bench_bulk("bulk-64B", 64, 100000 / k);
    bench_bulk("bulk-4KiB", 4096, 20000 / k);
    bench_mixed("mixed-16-256B", 50000 / k);
    bench_churn("churn-16KiB", 16384, 20000 / k);
    bench_threads("threads-4x-64B", 4, 500000 / k);

    al_teardown_thread();
#if defined(ALLOC_MIMENTO3) || defined(ALLOC_MIMENTO221)
    memento_shutdown();
#elif defined(ALLOC_RPMALLOC)
    rpmalloc_finalize();
#endif
    return 0;
}
