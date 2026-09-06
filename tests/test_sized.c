/*
 * Memento Sized-API Test Suite
 *
 * The sized family (memento_malloc / memento_free / ...) carries a 16-byte
 * header so free needs nothing but the pointer, and ownership is resolved
 * from the pointer itself — which makes cross-thread free(p) correct. This
 * file tests the family from the default (exact-size) build; the
 * MEMENTO_SIZED=1 build mode is covered by test_sized_mode.c.
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

#if !defined(_WIN32)
#include <pthread.h>
#endif

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  %-40s ", #name); \
    fflush(stdout); \
    test_##name(); \
    passed++; \
    printf("PASS\n"); \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL\n    Assertion: %s\n    File: %s:%d\n", \
               #cond, __FILE__, __LINE__); \
        failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NE(a, b) ASSERT((a) != (b))
#define ASSERT_NULL(p) ASSERT((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT((p) != NULL)
#define ASSERT_GE(a, b) ASSERT((a) >= (b))
#define ASSERT_LE(a, b) ASSERT((a) <= (b))

static int passed = 0;
static int failed = 0;

/* ============================================================================
 * Sized basics
 * ============================================================================ */

TEST(malloc_free_small) {
    for (int i = 0; i < 1000; i++) {
        size_t size = (size_t)(i % 500) + 1;
        void* p = memento_malloc(size);
        ASSERT_NOT_NULL(p);
        ASSERT_GE(memento_usable_size(p), size);
        memset(p, i & 0xFF, size);
        memento_free(p);
    }
}

TEST(malloc_free_large_and_huge) {
    size_t sizes[] = { 9000, 16384, 100 * 1024, 250 * 1024, 300 * 1024,
                       1024 * 1024, 3 * 1024 * 1024 };
    for (int i = 0; i < 7; i++) {
        void* p = memento_malloc(sizes[i]);
        ASSERT_NOT_NULL(p);
        ASSERT_GE(memento_usable_size(p), sizes[i]);
        memset(p, 0x5A, sizes[i]);
        memento_free(p);
    }
}

TEST(calloc_zeroes) {
    int* p = (int*)memento_calloc(1024, sizeof(int));
    ASSERT_NOT_NULL(p);
    for (int i = 0; i < 1024; i++) {
        ASSERT_EQ(p[i], 0);
    }
    memento_free(p);

    /* overflow must fail, not wrap (via volatile so the compiler's
     * alloc_size knowledge doesn't diagnose the constant) */
    volatile size_t huge_count = (size_t)-1;
    ASSERT_NULL(memento_calloc(huge_count, 2));
}

TEST(realloc_chains) {
    /* NULL → alloc, grow, shrink, to-zero → free */
    char* p = (char*)memento_realloc(NULL, 64);
    ASSERT_NOT_NULL(p);
    memset(p, 1, 64);

    p = (char*)memento_realloc(p, 4096);
    ASSERT_NOT_NULL(p);
    for (int i = 0; i < 64; i++) ASSERT_EQ(p[i], 1);

    p = (char*)memento_realloc(p, 200 * 1024);
    ASSERT_NOT_NULL(p);
    for (int i = 0; i < 64; i++) ASSERT_EQ(p[i], 1);

    p = (char*)memento_realloc(p, 32); /* shrink within usable: in place */
    ASSERT_NOT_NULL(p);
    for (int i = 0; i < 32; i++) ASSERT_EQ(p[i], 1);

    p = (char*)memento_realloc(p, 0);
    ASSERT_NULL(p);
}

TEST(realloc_in_place_hysteresis) {
    /* Growing within the usable size must not move the block. */
    char* p = (char*)memento_malloc(1000);
    ASSERT_NOT_NULL(p);
    size_t usable = memento_usable_size(p);
    char* q = (char*)memento_realloc(p, usable); /* fits exactly */
    ASSERT(q == p);
    memento_free(q);
}

TEST(aligned_variants) {
    size_t aligns[] = {16, 32, 64, 128, 256, 512, 4096};
    for (int i = 0; i < 7; i++) {
        size_t align = aligns[i];
        /* across small / page-run / huge tiers */
        size_t sizes[] = {8, 1000, 40000, 300 * 1024};
        for (int j = 0; j < 4; j++) {
            void* p = memento_aligned_alloc(align, sizes[j]);
            ASSERT_NOT_NULL(p);
            ASSERT(((uintptr_t)p & (align - 1)) == 0);
            ASSERT_GE(memento_usable_size(p), sizes[j]);
            memset(p, 0x11, sizes[j]);
            memento_free(p);
        }
    }
    /* musl SIMD case from the README: 64-byte aligned, page-ish buffers */
    void* simd = memento_aligned_alloc(64, 2048);
    ASSERT_NOT_NULL(simd);
    ASSERT(((uintptr_t)simd & 63) == 0);
    memento_free(simd);
}

TEST(usable_size_monotonic) {
    /* usable_size must always cover the request, and land in the class the
     * header describes. */
    size_t prev = 0;
    for (size_t s = 1; s <= 8192; s += 97) {
        void* p = memento_malloc(s);
        ASSERT_NOT_NULL(p);
        size_t u = memento_usable_size(p);
        ASSERT_GE(u, s);
        /* Slack is bounded by the worst class gap: coarse small classes
         * (6K->8K) leave ~2K, and once total crosses 8K the page-run tier
         * rounds up to a 4K page, leaving just under 4K. */
        ASSERT_LE(u, s + 16 + 4096);
        ASSERT_GE(u + 1, prev);    /* non-decreasing-ish as size grows */
        prev = u;
        memento_free(p);
    }
}

TEST(good_size_matches_usable) {
    size_t probe[] = {1, 17, 100, 1000, 8000, 9000, 100 * 1024, 1 << 20};
    for (int i = 0; i < 8; i++) {
        size_t g = memento_good_size(probe[i]);
        ASSERT_GE(g, probe[i]);
        void* p = memento_malloc(probe[i]);
        ASSERT_NOT_NULL(p);
        /* usable_size includes alignment slack, so it is >= good_size - 16 */
        ASSERT_GE(memento_usable_size(p) + 16, g);
        memento_free(p);
    }
}

/* ============================================================================
 * Cross-thread routing: free(p) from the WRONG thread must route home
 * ============================================================================ */

#if !defined(_WIN32)

typedef struct {
    void* ptrs[256];
    int count;
    volatile int go;
} handoff_t;

static void* foreign_free_worker(void* arg) {
    handoff_t* h = (handoff_t*)arg;
    while (!h->go) { /* spin briefly until main fills the batch */ }
    for (int i = 0; i < h->count; i++) {
        /* No heap, no size — the pointer knows the way home. */
        memento_free(h->ptrs[i]);
    }
    return NULL;
}

TEST(cross_thread_free_without_size) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_heap_stats_t before;
    memento_thread_heap_stats(heap, &before);

    handoff_t h;
    memset(&h, 0, sizeof(h));
    for (int i = 0; i < 256; i++) {
        h.ptrs[i] = memento_malloc(64 + (size_t)i);
        ASSERT_NOT_NULL(h.ptrs[i]);
        memset(h.ptrs[i], i, 64);
    }
    h.count = 256;

    pthread_t t;
    ASSERT_EQ(pthread_create(&t, NULL, foreign_free_worker, &h), 0);
    h.go = 1;
    pthread_join(t, NULL);

    /* Owner drains */
    memento_thread_heap_flush(heap);

    memento_heap_stats_t after;
    memento_thread_heap_stats(heap, &after);
    #if MEMENTO_STATS
    ASSERT_GE(after.foreign_free_count, before.foreign_free_count + 256);
#endif
}

TEST(cross_thread_free_large) {
    /* Large blocks carry their owner in the meta header — same guarantee. */
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_heap_stats_t before;
    memento_thread_heap_stats(heap, &before);

    handoff_t h;
    memset(&h, 0, sizeof(h));
    for (int i = 0; i < 16; i++) {
        h.ptrs[i] = memento_malloc((size_t)(16 + i) * 1024);
        ASSERT_NOT_NULL(h.ptrs[i]);
    }
    h.count = 16;

    pthread_t t;
    ASSERT_EQ(pthread_create(&t, NULL, foreign_free_worker, &h), 0);
    h.go = 1;
    pthread_join(t, NULL);

    memento_thread_heap_flush(heap);

    memento_heap_stats_t after;
    memento_thread_heap_stats(heap, &after);
    #if MEMENTO_STATS
    ASSERT_GE(after.foreign_free_count, before.foreign_free_count + 16);
#endif
}

static void* foreign_realloc_worker(void* arg) {
    handoff_t* h = (handoff_t*)arg;
    while (!h->go) {}
    /* Realloc a block owned by MAIN's heap from here: the header names the
     * owner, so this takes the alloc-here/copy/foreign-free path. */
    void* np = memento_realloc(h->ptrs[0], 5000);
    if (np) {
        /* Contents must survive the trip. */
        for (int i = 0; i < 100; i++) {
            if (((unsigned char*)np)[i] != 0xAB) { h->ptrs[1] = NULL; break; }
        }
        h->ptrs[0] = np;
    } else {
        h->ptrs[1] = NULL;
    }
    return NULL;
}

TEST(cross_thread_realloc_copy_path) {
    handoff_t h;
    memset(&h, 0, sizeof(h));
    h.ptrs[0] = memento_malloc(100);
    ASSERT_NOT_NULL(h.ptrs[0]);
    memset(h.ptrs[0], 0xAB, 100);
    h.ptrs[1] = h.ptrs[0]; /* worker clears this on content mismatch */

    pthread_t t;
    ASSERT_EQ(pthread_create(&t, NULL, foreign_realloc_worker, &h), 0);
    h.go = 1;
    pthread_join(t, NULL);

    ASSERT_NOT_NULL(h.ptrs[1]);           /* contents verified by worker */
    ASSERT_NOT_NULL(h.ptrs[0]);
    ASSERT_GE(memento_usable_size(h.ptrs[0]), 5000);

    /* Old block was foreign-freed onto our stack; drain it. */
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_heap_stats_t before;
    memento_thread_heap_stats(heap, &before);
    memento_thread_heap_flush(heap);
    memento_heap_stats_t after;
    memento_thread_heap_stats(heap, &after);
    #if MEMENTO_STATS
    ASSERT_GE(after.foreign_free_count, before.foreign_free_count + 1);
#endif

    memento_free(h.ptrs[0]);
}

TEST(report_shows_foreign_pending) {
    /* Between the worker's free and our flush, the report must show the
     * blocks parked on the MPSC stack. */
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_thread_heap_flush(heap); /* start clean */

    handoff_t h;
    memset(&h, 0, sizeof(h));
    for (int i = 0; i < 64; i++) {
        h.ptrs[i] = memento_malloc(48 + (size_t)i);
        ASSERT_NOT_NULL(h.ptrs[i]);
    }
    h.count = 64;

    pthread_t t;
    ASSERT_EQ(pthread_create(&t, NULL, foreign_free_worker, &h), 0);
    h.go = 1;
    pthread_join(t, NULL);

    FILE* tmp = tmpfile();
    ASSERT_NOT_NULL(tmp);
    memento_thread_heap_report(heap, tmp);
    fflush(tmp);
    rewind(tmp);
    char line[256];
    size_t pending = 0;
    int found = 0;
    while (fgets(line, sizeof(line), tmp)) {
        char* tag = strstr(line, "foreign pending");
        if (tag) {
            size_t n = 0;
            if (sscanf(tag + strlen("foreign pending"), "%zu", &n) == 1) {
                pending = n;
                found = 1;
            }
        }
    }
    fclose(tmp);
    ASSERT(found);
    ASSERT_GE(pending, 64);

    memento_thread_heap_flush(heap);
}

#endif /* !_WIN32 */

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("=== Memento Sized-API Test Suite ===\n\n");

    if (!memento_init()) {
        printf("Failed to initialize Memento\n");
        return 1;
    }

    RUN_TEST(malloc_free_small);
    RUN_TEST(malloc_free_large_and_huge);
    RUN_TEST(calloc_zeroes);
    RUN_TEST(realloc_chains);
    RUN_TEST(realloc_in_place_hysteresis);
    RUN_TEST(aligned_variants);
    RUN_TEST(usable_size_monotonic);
    RUN_TEST(good_size_matches_usable);
#if !defined(_WIN32)
    RUN_TEST(cross_thread_free_without_size);
    RUN_TEST(cross_thread_free_large);
    RUN_TEST(cross_thread_realloc_copy_path);
    RUN_TEST(report_shows_foreign_pending);
#endif

    memento_shutdown();

    printf("\n=== Results ===\n");
    printf("  Passed: %d\n", passed);
    printf("  Failed: %d\n", failed);

    if (failed == 0) {
        printf("\n✓ All tests passed!\n");
        return 0;
    }
    printf("\n✗ Some tests failed!\n");
    return 1;
}
