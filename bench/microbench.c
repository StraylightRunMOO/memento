/*
 * Lightweight single-header microbench for Memento only.
 * Build: cc -O2 -Iinclude microbench.c -o microbench -lpthread
 *
 * Prints Mops/s for several patterns so we can compare trees.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#define MEMENTO_IMPLEMENTATION
#include "memento.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void bench_fixed(const char* name, memento_thread_heap_t* heap,
                        size_t size, int iters) {
    void** ptrs = (void**)malloc((size_t)iters * sizeof(void*));
    if (!ptrs) {
        fprintf(stderr, "oom\n");
        exit(1);
    }

    /* Warmup */
    for (int i = 0; i < 1000; i++) {
        void* p = memento_thread_heap_alloc(heap, size);
        memento_thread_heap_free(heap, p, size);
    }

    double t0 = now_sec();
    for (int i = 0; i < iters; i++) {
        ptrs[i] = memento_thread_heap_alloc(heap, size);
    }
    for (int i = iters - 1; i >= 0; i--) {
        memento_thread_heap_free(heap, ptrs[i], size);
    }
    double t1 = now_sec();

    /* Alloc+free pairs counted as 2 ops each iteration? Report alloc+free cycle. */
    double cycles = (double)iters;
    double sec = t1 - t0;
    double mops = (cycles / sec) / 1e6;
    double ns = (sec * 1e9) / cycles;
    printf("  %-28s %8.2f Mops/s  (%6.1f ns/cycle)\n", name, mops, ns);
    free(ptrs);
}

static void bench_churn(const char* name, memento_thread_heap_t* heap,
                        size_t size, int iters) {
    /* Immediate free — pure freelist hit path after warmup */
    for (int i = 0; i < 1000; i++) {
        void* p = memento_thread_heap_alloc(heap, size);
        memento_thread_heap_free(heap, p, size);
    }
    double t0 = now_sec();
    for (int i = 0; i < iters; i++) {
        void* p = memento_thread_heap_alloc(heap, size);
        memento_thread_heap_free(heap, p, size);
    }
    double t1 = now_sec();
    double sec = t1 - t0;
    double mops = ((double)iters / sec) / 1e6;
    double ns = (sec * 1e9) / (double)iters;
    printf("  %-28s %8.2f Mops/s  (%6.1f ns/cycle)\n", name, mops, ns);
}

static void bench_mixed(const char* name, memento_thread_heap_t* heap, int iters) {
    static const size_t sizes[] = {32, 48, 64, 96, 128, 256, 512, 1024, 2048, 4096};
    const int nsizes = (int)(sizeof(sizes) / sizeof(sizes[0]));
    void** ptrs = (void**)malloc((size_t)iters * sizeof(void*));
    size_t* sz = (size_t*)malloc((size_t)iters * sizeof(size_t));
    if (!ptrs || !sz) exit(1);
    for (int i = 0; i < iters; i++) {
        sz[i] = sizes[i % nsizes];
    }
    /* Warmup: carve every class and park the emptied spans, so the timed
     * round measures the steady state (recycling spans) rather than the
     * one-time cost of first-touch page faults on fresh mappings. */
    for (int i = 0; i < iters; i++) {
        ptrs[i] = memento_thread_heap_alloc(heap, sz[i]);
    }
    for (int i = iters - 1; i >= 0; i--) {
        memento_thread_heap_free(heap, ptrs[i], sz[i]);
    }
    double t0 = now_sec();
    for (int i = 0; i < iters; i++) {
        ptrs[i] = memento_thread_heap_alloc(heap, sz[i]);
    }
    for (int i = iters - 1; i >= 0; i--) {
        memento_thread_heap_free(heap, ptrs[i], sz[i]);
    }
    double t1 = now_sec();
    double sec = t1 - t0;
    double mops = ((double)iters / sec) / 1e6;
    double ns = (sec * 1e9) / (double)iters;
    printf("  %-28s %8.2f Mops/s  (%6.1f ns/cycle)\n", name, mops, ns);
    free(ptrs);
    free(sz);
}

static void bench_large_churn(const char* name, memento_thread_heap_t* heap,
                              size_t size, int iters) {
    for (int i = 0; i < 100; i++) {
        void* p = memento_thread_heap_alloc(heap, size);
        memento_thread_heap_free(heap, p, size);
    }
    double t0 = now_sec();
    for (int i = 0; i < iters; i++) {
        void* p = memento_thread_heap_alloc(heap, size);
        memento_thread_heap_free(heap, p, size);
    }
    double t1 = now_sec();
    double sec = t1 - t0;
    double mops = ((double)iters / sec) / 1e6;
    double ns = (sec * 1e9) / (double)iters;
    printf("  %-28s %8.2f Mops/s  (%6.1f ns/cycle)\n", name, mops, ns);
}

int main(void) {
    printf("Memento microbench  version=%s\n", memento_version_string());
    if (!memento_init()) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    memento_thread_heap_t* heap = memento_thread_heap_get();
    if (!heap) {
        fprintf(stderr, "heap failed\n");
        return 1;
    }

    printf("\nFreelist hit (alloc+free same size):\n");
    bench_churn("churn 64B", heap, 64, 2000000);
    bench_churn("churn 256B", heap, 256, 2000000);
    bench_churn("churn 1024B", heap, 1024, 1000000);

    printf("\nBulk alloc then bulk free:\n");
    bench_fixed("bulk 64B x100k", heap, 64, 100000);
    bench_fixed("bulk 256B x100k", heap, 256, 100000);
    bench_fixed("bulk 4096B x20k", heap, 4096, 20000);

    printf("\nMixed size classes:\n");
    bench_mixed("mixed 32-4096 x50k", heap, 50000);

    printf("\nLarge object path (LOC after first free):\n");
    bench_large_churn("large 16KB churn x20k", heap, 16 * 1024, 20000);
    bench_large_churn("large 64KB churn x5k", heap, 64 * 1024, 5000);

    memento_shutdown();
    return 0;
}
