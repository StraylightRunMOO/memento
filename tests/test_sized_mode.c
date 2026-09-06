/*
 * Memento MEMENTO_SIZED=1 Mode Test Suite
 *
 * Compiled with -DMEMENTO_SIZED=1: the thread-heap API itself carries the
 * 16-byte header, free() may ignore the size argument, and the global
 * memento_free(p) works on thread-heap allocations. This is the build you
 * LD_PRELOAD into third-party code that "doesn't know the size".
 */

#define MEMENTO_IMPLEMENTATION
#include "memento.h"

#if !MEMENTO_SIZED
#error "test_sized_mode must be compiled with -DMEMENTO_SIZED=1"
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

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NULL(p) ASSERT((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT((p) != NULL)
#define ASSERT_GE(a, b) ASSERT((a) >= (b))

static int passed = 0;
static int failed = 0;

TEST(heap_alloc_free_sized) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    /* free with a bogus size: header is authoritative in sized mode */
    for (int i = 0; i < 500; i++) {
        size_t size = (size_t)(i % 1000) + 1;
        void* p = memento_thread_heap_alloc(heap, size);
        ASSERT_NOT_NULL(p);
        memset(p, i & 0xFF, size);
        memento_thread_heap_free(heap, p, 0); /* wrong size on purpose */
    }
}

TEST(heap_alloc_sized_all_tiers) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    size_t sizes[] = { 8, 100, 4096, 8192, 9000, 64 * 1024, 200 * 1024,
                       512 * 1024 };
    for (int i = 0; i < 8; i++) {
        void* p = memento_thread_heap_alloc(heap, sizes[i]);
        ASSERT_NOT_NULL(p);
        memset(p, 0x33, sizes[i]);
        /* and the global free() accepts heap pointers in sized mode */
        memento_free(p);
    }
}

TEST(heap_realloc_sized) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    char* p = (char*)memento_thread_heap_alloc(heap, 128);
    ASSERT_NOT_NULL(p);
    memset(p, 4, 128);
    /* old_size is ignored in sized mode — pass garbage */
    p = (char*)memento_thread_heap_realloc(heap, p, 999999, 8192);
    ASSERT_NOT_NULL(p);
    for (int i = 0; i < 128; i++) ASSERT_EQ(p[i], 4);
    memento_thread_heap_free(heap, p, 0);
}

TEST(heap_aligned_sized) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    void* p = memento_thread_heap_alloc_aligned(heap, 2048, 64);
    ASSERT_NOT_NULL(p);
    ASSERT(((uintptr_t)p & 63) == 0);
    memset(p, 1, 2048);
    memento_thread_heap_free_aligned(heap, p, 0, 0); /* ignored in sized mode */
}

TEST(containers_still_work_sized) {
    /* Pool/arena/slab route through the thread-heap API; in sized mode they
     * must remain consistent. */
    memento_thread_heap_t* heap = memento_thread_heap_get();

    memento_pool_t* pool = memento_pool_create(64, 100, heap);
    ASSERT_NOT_NULL(pool);
    void* objs[100];
    for (int i = 0; i < 100; i++) {
        objs[i] = memento_pool_alloc(pool);
        ASSERT_NOT_NULL(objs[i]);
    }
    for (int i = 0; i < 100; i++) {
        memento_pool_free(pool, objs[i]);
    }
    memento_pool_destroy(pool);

    memento_arena_t* arena = memento_arena_create(8192, heap);
    ASSERT_NOT_NULL(arena);
    for (int i = 0; i < 50; i++) {
        ASSERT_NOT_NULL(memento_arena_alloc(arena, 100, 16));
    }
    memento_arena_reset(arena);
    memento_arena_destroy(arena);

    memento_slab_t* slab = memento_slab_create(heap);
    ASSERT_NOT_NULL(slab);
    void* p = memento_slab_alloc(slab, 200);
    ASSERT_NOT_NULL(p);
    memento_slab_free(slab, p, 200);
    memento_slab_destroy(slab);
}

int main(void) {
    printf("=== Memento SIZED-Mode Test Suite ===\n\n");

    if (!memento_init()) {
        printf("Failed to initialize Memento\n");
        return 1;
    }

    RUN_TEST(heap_alloc_free_sized);
    RUN_TEST(heap_alloc_sized_all_tiers);
    RUN_TEST(heap_realloc_sized);
    RUN_TEST(heap_aligned_sized);
    RUN_TEST(containers_still_work_sized);

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
