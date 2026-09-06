/*
 * Memento Debug-Mode Test Suite
 *
 * Compiled with -DMEMENTO_DEBUG=1 (canaries, double-free tripwires,
 * ASan poisoning when available). Exercises the debug paths without tripping
 * them — plus the interplay that matters: debug canaries and sized headers
 * sharing the same blocks.
 */

#define MEMENTO_IMPLEMENTATION
#include "memento.h"

#if !MEMENTO_DEBUG
#error "test_debug must be compiled with -DMEMENTO_DEBUG=1"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

#define ASSERT_NOT_NULL(p) ASSERT((p) != NULL)
#define ASSERT_GE(a, b) ASSERT((a) >= (b))

static int passed = 0;
static int failed = 0;

TEST(debug_churn_all_classes) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    /* Hammer every class: canary write/clear on every cycle. */
    for (int round = 0; round < 20; round++) {
        for (size_t sc = 0; sc < MEMENTO_SIZE_CLASS_COUNT; sc++) {
            size_t size = memento_size_class_to_size(sc);
            void* p = memento_thread_heap_alloc(heap, size);
            ASSERT_NOT_NULL(p);
            memset(p, 0xCD, size);
            memento_thread_heap_free(heap, p, size);
        }
    }
}

TEST(debug_sized_and_exact_share_heap) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    /* Alternate exact and sized traffic on the same heap/classes; the debug
     * canary and the sized header coexist by design. */
    for (int i = 0; i < 200; i++) {
        void* a = memento_thread_heap_alloc(heap, 100);
        void* b = memento_heap_malloc(heap, 100);
        ASSERT_NOT_NULL(a);
        ASSERT_NOT_NULL(b);
        memset(a, 1, 100);
        memset(b, 2, 100);
        memento_thread_heap_free(heap, a, 100);
        memento_heap_mfree(heap, b);
    }
}

TEST(debug_page_runs_and_huge) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    void* p1 = memento_thread_heap_alloc(heap, 50000);
    void* p2 = memento_thread_heap_alloc(heap, 1024 * 1024);
    ASSERT_NOT_NULL(p1);
    ASSERT_NOT_NULL(p2);
    memset(p1, 1, 50000);
    memset(p2, 2, 1024 * 1024);
    memento_thread_heap_free(heap, p1, 50000);
    memento_thread_heap_free(heap, p2, 1024 * 1024);
}

TEST(debug_proxies_default_on) {
    /* MEMENTO_PROXY_DEBUG defaults to MEMENTO_DEBUG, so this build has
     * instrumented proxies without any extra define. */
#if MEMENTO_PROXY_DEBUG
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_proxy_t* px = memento_proxy_wrap_heap(heap, MEMENTO_PROXY_DEBUG_ALL);
    ASSERT_NOT_NULL(px);
    void* p = memento_proxy_alloc(px, 333);
    ASSERT_NOT_NULL(p);
    ASSERT_GE(memento_proxy_outstanding_count(px), 1);
    memento_proxy_free(px, p);
    memento_proxy_destroy(px);
#endif
}

int main(void) {
    printf("=== Memento Debug-Mode Test Suite ===\n\n");

    if (!memento_init()) {
        printf("Failed to initialize Memento\n");
        return 1;
    }

    RUN_TEST(debug_churn_all_classes);
    RUN_TEST(debug_sized_and_exact_share_heap);
    RUN_TEST(debug_page_runs_and_huge);
    RUN_TEST(debug_proxies_default_on);

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
