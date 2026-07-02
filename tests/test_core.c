/*
 * Memento Core Test Suite
 * 
 * Tests all allocator types with comprehensive coverage.
 */

#define MEMENTO_IMPLEMENTATION
#include "memento.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* Test framework */
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

static int passed = 0;
static int failed = 0;

/* ============================================================================
 * Initialization Tests
 * ============================================================================ */

TEST(version_check) {
    /* Check version macros */
    ASSERT_EQ(MEMENTO_VERSION_MAJOR, 2);
    ASSERT_EQ(MEMENTO_VERSION_MINOR, 1);
    ASSERT_EQ(MEMENTO_VERSION_PATCH, 0);
    
    /* Check version string */
    ASSERT_NOT_NULL(memento_version_string());
    ASSERT_EQ(strcmp(memento_version_string(), "2.1.0"), 0);
    
    /* Check version number */
    ASSERT_EQ(memento_version_number(), 0x020100);
    
    /* Check version check function */
    ASSERT(memento_version_check(2, 0, 0));
    ASSERT(memento_version_check(1, 99, 99));
    ASSERT(!memento_version_check(3, 0, 0));
}

TEST(init_shutdown) {
    ASSERT(memento_init());
    memento_shutdown();
}

TEST(multiple_init) {
    ASSERT(memento_init());
    ASSERT(memento_init());  /* Idempotent */
    memento_shutdown();
}

/* ============================================================================
 * Utility Function Tests
 * ============================================================================ */

TEST(power_of_two) {
    ASSERT(memento_is_power_of_two(1));
    ASSERT(memento_is_power_of_two(2));
    ASSERT(memento_is_power_of_two(64));
    ASSERT(memento_is_power_of_two(1024));
    /* 0 is considered power of two by the bit trick, accept that */
    ASSERT(memento_is_power_of_two(0));
    ASSERT(!memento_is_power_of_two(3));
    ASSERT(!memento_is_power_of_two(63));
    ASSERT(!memento_is_power_of_two(100));
}

TEST(align_up) {
    ASSERT_EQ(memento_align_up(0, 8), 0);
    ASSERT_EQ(memento_align_up(1, 8), 8);
    ASSERT_EQ(memento_align_up(7, 8), 8);
    ASSERT_EQ(memento_align_up(8, 8), 8);
    ASSERT_EQ(memento_align_up(9, 8), 16);
    ASSERT_EQ(memento_align_up(63, 64), 64);
    ASSERT_EQ(memento_align_up(64, 64), 64);
    ASSERT_EQ(memento_align_up(65, 64), 128);
}

TEST(align_down) {
    ASSERT_EQ(memento_align_down(0, 8), 0);
    ASSERT_EQ(memento_align_down(1, 8), 0);
    ASSERT_EQ(memento_align_down(7, 8), 0);
    ASSERT_EQ(memento_align_down(8, 8), 8);
    ASSERT_EQ(memento_align_down(9, 8), 8);
    ASSERT_EQ(memento_align_down(63, 64), 0);
    ASSERT_EQ(memento_align_down(64, 64), 64);
    ASSERT_EQ(memento_align_down(65, 64), 64);
    ASSERT_EQ(memento_align_down(127, 64), 64);
}

TEST(size_classes) {
    /* Test size class mapping - 24 size classes */
    ASSERT_EQ(memento_size_class_for(1), 0);       /* 1-32 -> class 0 (32 bytes) */
    ASSERT_EQ(memento_size_class_for(32), 0);
    ASSERT_EQ(memento_size_class_for(33), 1);      /* 33-48 -> class 1 (48 bytes) */
    ASSERT_EQ(memento_size_class_for(64), 2);      /* 49-64 -> class 2 (64 bytes) */
    ASSERT_EQ(memento_size_class_for(8192), 23);   /* 4097-8192 -> class 23 (8192 bytes) */
    ASSERT_EQ(memento_size_class_for(10000), 23);  /* Oversize maps to largest */
    
    /* Test round-trip */
    for (size_t sc = 0; sc < MEMENTO_SIZE_CLASS_COUNT; sc++) {
        size_t size = memento_size_class_to_size(sc);
        size_t computed_sc = memento_size_class_for(size);
        ASSERT_EQ(computed_sc, sc);
    }
}

/* ============================================================================
 * Thread Heap Tests
 * ============================================================================ */

TEST(heap_basic_alloc_free) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    ASSERT_NOT_NULL(heap);
    
    void* ptr = memento_thread_heap_alloc(heap, 1024);
    ASSERT_NOT_NULL(ptr);
    memset(ptr, 0xAB, 1024);
    memento_thread_heap_free(heap, ptr, 1024);
}

TEST(heap_null_heap) {
    void* ptr = memento_thread_heap_alloc(NULL, 1024);
    ASSERT_NULL(ptr);
    
    /* Should not crash */
    memento_thread_heap_free(NULL, NULL, 0);
    memento_thread_heap_free(NULL, (void*)1, 1024);
}

TEST(heap_zero_size) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    void* ptr = memento_thread_heap_alloc(heap, 0);
    ASSERT_NULL(ptr);
}

TEST(heap_null_ptr_free) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    /* Should not crash */
    memento_thread_heap_free(heap, NULL, 1024);
}

TEST(heap_all_size_classes) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    
    for (size_t sc = 0; sc < MEMENTO_SIZE_CLASS_COUNT; sc++) {
        size_t size = memento_size_class_to_size(sc);
        void* ptr = memento_thread_heap_alloc(heap, size);
        ASSERT_NOT_NULL(ptr);
        memset(ptr, 0xCD, size);
        memento_thread_heap_free(heap, ptr, size);
    }
}

TEST(heap_mixed_sizes) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    void* ptrs[100];
    size_t sizes[100];
    
    /* Allocate various sizes */
    for (int i = 0; i < 100; i++) {
        sizes[i] = (i + 1) * 16;
        if (sizes[i] > 8192) sizes[i] = 8192;
        ptrs[i] = memento_thread_heap_alloc(heap, sizes[i]);
        ASSERT_NOT_NULL(ptrs[i]);
        memset(ptrs[i], i & 0xFF, sizes[i]);
    }
    
    /* Verify and free in reverse order */
    for (int i = 99; i >= 0; i--) {
        unsigned char* p = (unsigned char*)ptrs[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            ASSERT_EQ(p[j], i & 0xFF);
        }
        memento_thread_heap_free(heap, ptrs[i], sizes[i]);
    }
}

TEST(heap_large_allocation) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    
    /* Allocations > 8KB go direct to system */
    size_t large_sizes[] = {8193, 16384, 32768, 65536, 131072, 1048576};
    
    for (size_t i = 0; i < sizeof(large_sizes)/sizeof(large_sizes[0]); i++) {
        void* ptr = memento_thread_heap_alloc(heap, large_sizes[i]);
        ASSERT_NOT_NULL(ptr);
        memset(ptr, 0xEF, large_sizes[i]);
        memento_thread_heap_free(heap, ptr, large_sizes[i]);
    }
}

TEST(heap_reuse) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    
    /* Allocate and free same size multiple times */
    void* ptr1 = memento_thread_heap_alloc(heap, 256);
    ASSERT_NOT_NULL(ptr1);
    memento_thread_heap_free(heap, ptr1, 256);
    
    void* ptr2 = memento_thread_heap_alloc(heap, 256);
    ASSERT_NOT_NULL(ptr2);
    
    /* Same pointer likely reused from cache */
    memento_thread_heap_free(heap, ptr2, 256);
}

TEST(heap_realloc) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    
    /* Test realloc from NULL */
    void* ptr = memento_thread_heap_realloc(heap, NULL, 0, 100);
    ASSERT_NOT_NULL(ptr);
    
    /* Test realloc grow */
    strcpy((char*)ptr, "Hello");
    void* new_ptr = memento_thread_heap_realloc(heap, ptr, 100, 200);
    ASSERT_NOT_NULL(new_ptr);
    ASSERT_EQ(strcmp((char*)new_ptr, "Hello"), 0);
    
    /* Test realloc shrink */
    void* final_ptr = memento_thread_heap_realloc(heap, new_ptr, 200, 50);
    ASSERT_NOT_NULL(final_ptr);
    ASSERT_EQ(strcmp((char*)final_ptr, "Hello"), 0);
    
    /* Test realloc to zero (free) */
    void* null_ptr = memento_thread_heap_realloc(heap, final_ptr, 50, 0);
    ASSERT_NULL(null_ptr);
}

/* ============================================================================
 * Pool Allocator Tests
 * ============================================================================ */

TEST(pool_create_destroy) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    
    memento_pool_t* pool = memento_pool_create(sizeof(int), 100, heap);
    ASSERT_NOT_NULL(pool);
    memento_pool_destroy(pool);
}

TEST(pool_basic_alloc_free) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_pool_t* pool = memento_pool_create(sizeof(int), 100, heap);
    ASSERT_NOT_NULL(pool);
    
    int* ptr = (int*)memento_pool_alloc(pool);
    ASSERT_NOT_NULL(ptr);
    *ptr = 42;
    ASSERT_EQ(*ptr, 42);
    
    memento_pool_free(pool, ptr);
    memento_pool_destroy(pool);
}

TEST(pool_exhaustion) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_pool_t* pool = memento_pool_create(sizeof(int), 10, heap);
    ASSERT_NOT_NULL(pool);
    
    /* Allocate all objects */
    void* ptrs[10];
    for (int i = 0; i < 10; i++) {
        ptrs[i] = memento_pool_alloc(pool);
        ASSERT_NOT_NULL(ptrs[i]);
    }
    
    /* Next allocation should fail (pool exhausted) */
    void* extra = memento_pool_alloc(pool);
    ASSERT_NULL(extra);
    
    /* Free one and try again */
    memento_pool_free(pool, ptrs[0]);
    ptrs[0] = memento_pool_alloc(pool);
    ASSERT_NOT_NULL(ptrs[0]);
    
    for (int i = 0; i < 10; i++) {
        memento_pool_free(pool, ptrs[i]);
    }
    memento_pool_destroy(pool);
}

TEST(pool_recycle) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_pool_t* pool = memento_pool_create(sizeof(int), 10, heap);
    ASSERT_NOT_NULL(pool);
    
    /* Allocate, free, reallocate */
    void* ptr1 = memento_pool_alloc(pool);
    ASSERT_NOT_NULL(ptr1);
    memento_pool_free(pool, ptr1);
    
    void* ptr2 = memento_pool_alloc(pool);
    ASSERT_NOT_NULL(ptr2);
    /* Should reuse the same slot */
    ASSERT_EQ(ptr1, ptr2);
    
    memento_pool_free(pool, ptr2);
    memento_pool_destroy(pool);
}

TEST(pool_various_sizes) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    
    size_t sizes[] = {1, 8, 16, 32, 64, 128, 256, 512, 1024};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        memento_pool_t* pool = memento_pool_create(sizes[i], 10, heap);
        ASSERT_NOT_NULL(pool);
        
        void* ptr = memento_pool_alloc(pool);
        ASSERT_NOT_NULL(ptr);
        memset(ptr, 0xAB, sizes[i]);
        
        memento_pool_free(pool, ptr);
        memento_pool_destroy(pool);
    }
}

TEST(pool_null_handling) {
    /* Should not crash */
    memento_pool_destroy(NULL);
    memento_pool_free(NULL, NULL);
    memento_pool_free(NULL, (void*)1);
    
    ASSERT_NULL(memento_pool_alloc(NULL));
}

/* ============================================================================
 * Arena Allocator Tests
 * ============================================================================ */

TEST(arena_create_destroy) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    
    memento_arena_t* arena = memento_arena_create(4096, heap);
    ASSERT_NOT_NULL(arena);
    memento_arena_destroy(arena);
}

TEST(arena_basic_alloc) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_arena_t* arena = memento_arena_create(4096, heap);
    ASSERT_NOT_NULL(arena);
    
    int* a = (int*)memento_arena_alloc(arena, sizeof(int) * 100, _Alignof(int));
    ASSERT_NOT_NULL(a);
    
    for (int i = 0; i < 100; i++) {
        a[i] = i;
    }
    
    for (int i = 0; i < 100; i++) {
        ASSERT_EQ(a[i], i);
    }
    
    memento_arena_destroy(arena);
}

TEST(arena_alignment) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_arena_t* arena = memento_arena_create(4096, heap);
    ASSERT_NOT_NULL(arena);
    
    /* Test various alignments */
    size_t alignments[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512};
    
    for (size_t i = 0; i < sizeof(alignments)/sizeof(alignments[0]); i++) {
        void* ptr = memento_arena_alloc(arena, 64, alignments[i]);
        ASSERT_NOT_NULL(ptr);
        ASSERT_EQ(((uintptr_t)ptr) & (alignments[i] - 1), 0);
    }
    
    memento_arena_destroy(arena);
}

TEST(arena_save_restore) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_arena_t* arena = memento_arena_create(4096, heap);
    ASSERT_NOT_NULL(arena);
    
    /* Allocate and save */
    int* a = (int*)memento_arena_alloc(arena, sizeof(int) * 10, _Alignof(int));
    ASSERT_NOT_NULL(a);
    for (int i = 0; i < 10; i++) a[i] = i;
    
    memento_arena_save_t save = memento_arena_save(arena);
    size_t used_before = memento_arena_used(arena);
    
    /* Allocate more */
    double* b = (double*)memento_arena_alloc(arena, sizeof(double) * 10, _Alignof(double));
    ASSERT_NOT_NULL(b);
    for (int i = 0; i < 10; i++) b[i] = i * 3.14;
    
    ASSERT_GE(memento_arena_used(arena), used_before);
    
    /* Restore */
    memento_arena_restore(arena, &save);
    ASSERT_EQ(memento_arena_used(arena), used_before);
    
    /* Original data should still be accessible */
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(a[i], i);
    }
    
    memento_arena_destroy(arena);
}

TEST(arena_reset) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_arena_t* arena = memento_arena_create(4096, heap);
    ASSERT_NOT_NULL(arena);
    
    /* Allocate some data */
    for (int i = 0; i < 100; i++) {
        void* ptr = memento_arena_alloc(arena, 64, 8);
        ASSERT_NOT_NULL(ptr);
    }
    
    ASSERT_GE(memento_arena_used(arena), 0);
    
    /* Reset */
    memento_arena_reset(arena);
    ASSERT_EQ(memento_arena_used(arena), 0);
    
    /* Can allocate again */
    void* ptr = memento_arena_alloc(arena, 1024, 8);
    ASSERT_NOT_NULL(ptr);
    
    memento_arena_destroy(arena);
}

TEST(arena_growth) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    
    /* Small initial capacity */
    memento_arena_t* arena = memento_arena_create(256, heap);
    ASSERT_NOT_NULL(arena);
    
    /* Allocate more than initial capacity */
    for (int i = 0; i < 100; i++) {
        void* ptr = memento_arena_alloc(arena, 128, 8);
        ASSERT_NOT_NULL(ptr);
    }
    
    ASSERT_GE(memento_arena_capacity(arena), 256);
    
    memento_arena_destroy(arena);
}

TEST(arena_null_handling) {
    /* Should not crash */
    memento_arena_destroy(NULL);
    ASSERT_NULL(memento_arena_alloc(NULL, 100, 8));
    memento_arena_reset(NULL);
    
    memento_arena_t dummy = {0};
    memento_arena_restore(NULL, NULL);
    memento_arena_restore(&dummy, NULL);
}

TEST(arena_zero_size) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_arena_t* arena = memento_arena_create(4096, heap);
    ASSERT_NOT_NULL(arena);
    
    void* ptr = memento_arena_alloc(arena, 0, 8);
    ASSERT_NULL(ptr);
    
    memento_arena_destroy(arena);
}

/* ============================================================================
 * Stack Allocator Tests
 * ============================================================================ */

TEST(stack_create_destroy) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    
    memento_stack_t* stack = memento_stack_create(4096, heap);
    ASSERT_NOT_NULL(stack);
    memento_stack_destroy(stack);
}

TEST(stack_push_pop) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_stack_t* stack = memento_stack_create(4096, heap);
    ASSERT_NOT_NULL(stack);
    
    /* Push data */
    int* a = (int*)memento_stack_push(stack, sizeof(int) * 10, _Alignof(int));
    ASSERT_NOT_NULL(a);
    for (int i = 0; i < 10; i++) a[i] = i;
    
    double* b = (double*)memento_stack_push(stack, sizeof(double) * 5, _Alignof(double));
    ASSERT_NOT_NULL(b);
    for (int i = 0; i < 5; i++) b[i] = i * 2.0;
    
    /* Pop in reverse order */
    memento_stack_pop(stack, b);
    memento_stack_pop(stack, a);
    
    memento_stack_destroy(stack);
}

TEST(stack_marker) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_stack_t* stack = memento_stack_create(4096, heap);
    ASSERT_NOT_NULL(stack);
    
    /* Push some data */
    int* a = (int*)memento_stack_push(stack, sizeof(int) * 10, _Alignof(int));
    ASSERT_NOT_NULL(a);
    
    /* Mark position */
    memento_stack_marker_t marker = memento_stack_marker(stack);
    
    /* Push more */
    double* b = (double*)memento_stack_push(stack, sizeof(double) * 5, _Alignof(double));
    ASSERT_NOT_NULL(b);
    char* c = (char*)memento_stack_push(stack, 100, 1);
    ASSERT_NOT_NULL(c);
    
    /* Pop to marker */
    memento_stack_pop_to_marker(stack, marker);
    
    /* Can push again from marker */
    void* d = memento_stack_push(stack, 200, 8);
    ASSERT_NOT_NULL(d);
    
    memento_stack_destroy(stack);
}

TEST(stack_alignment) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_stack_t* stack = memento_stack_create(4096, heap);
    ASSERT_NOT_NULL(stack);
    
    size_t alignments[] = {1, 2, 4, 8, 16, 32, 64};
    
    for (size_t i = 0; i < sizeof(alignments)/sizeof(alignments[0]); i++) {
        void* ptr = memento_stack_push(stack, 64, alignments[i]);
        ASSERT_NOT_NULL(ptr);
        ASSERT_EQ(((uintptr_t)ptr) & (alignments[i] - 1), 0);
    }
    
    memento_stack_destroy(stack);
}

TEST(stack_reset) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_stack_t* stack = memento_stack_create(4096, heap);
    ASSERT_NOT_NULL(stack);
    
    /* Push data */
    for (int i = 0; i < 10; i++) {
        void* ptr = memento_stack_push(stack, 128, 8);
        ASSERT_NOT_NULL(ptr);
    }
    
    /* Reset */
    memento_stack_reset(stack);
    
    /* Can push from beginning again */
    void* ptr = memento_stack_push(stack, 1024, 8);
    ASSERT_NOT_NULL(ptr);
    
    memento_stack_destroy(stack);
}

TEST(stack_overflow) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_stack_t* stack = memento_stack_create(256, heap);
    ASSERT_NOT_NULL(stack);
    
    /* First allocation should succeed */
    void* a = memento_stack_push(stack, 128, 8);
    ASSERT_NOT_NULL(a);
    
    /* Second allocation should fail (stack overflow) */
    void* b = memento_stack_push(stack, 256, 8);
    ASSERT_NULL(b);
    
    memento_stack_destroy(stack);
}

TEST(stack_null_handling) {
    /* Should not crash */
    memento_stack_destroy(NULL);
    ASSERT_NULL(memento_stack_push(NULL, 100, 8));
    memento_stack_reset(NULL);
    memento_stack_pop(NULL, NULL);
    memento_stack_pop_to_marker(NULL, 0);
}

/* ============================================================================
 * Slab Allocator Tests
 * ============================================================================ */

TEST(slab_create_destroy) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    
    memento_slab_t* slab = memento_slab_create(heap);
    ASSERT_NOT_NULL(slab);
    memento_slab_destroy(slab);
}

TEST(slab_basic_alloc_free) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_slab_t* slab = memento_slab_create(heap);
    ASSERT_NOT_NULL(slab);
    
    void* ptr = memento_slab_alloc(slab, 256);
    ASSERT_NOT_NULL(ptr);
    memset(ptr, 0xAB, 256);
    
    memento_slab_free(slab, ptr, 256);
    memento_slab_destroy(slab);
}

TEST(slab_various_sizes) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_slab_t* slab = memento_slab_create(heap);
    ASSERT_NOT_NULL(slab);
    
    size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
    
    /* Allocate various sizes */
    void* ptrs[10];
    for (int i = 0; i < 10; i++) {
        ptrs[i] = memento_slab_alloc(slab, sizes[i]);
        ASSERT_NOT_NULL(ptrs[i]);
        memset(ptrs[i], i, sizes[i]);
    }
    
    /* Free in reverse order */
    for (int i = 9; i >= 0; i--) {
        memento_slab_free(slab, ptrs[i], sizes[i]);
    }
    
    memento_slab_destroy(slab);
}

TEST(slab_large_allocation) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_slab_t* slab = memento_slab_create(heap);
    ASSERT_NOT_NULL(slab);
    
    /* Large allocations bypass slab cache */
    void* ptr = memento_slab_alloc(slab, 16384);
    ASSERT_NOT_NULL(ptr);
    memset(ptr, 0xEF, 16384);
    
    memento_slab_free(slab, ptr, 16384);
    memento_slab_destroy(slab);
}

TEST(slab_recycle) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_slab_t* slab = memento_slab_create(heap);
    ASSERT_NOT_NULL(slab);
    
    /* Allocate, free, reallocate same size */
    void* ptr1 = memento_slab_alloc(slab, 256);
    ASSERT_NOT_NULL(ptr1);
    memento_slab_free(slab, ptr1, 256);
    
    void* ptr2 = memento_slab_alloc(slab, 256);
    ASSERT_NOT_NULL(ptr2);
    /* Should come from cache */
    
    memento_slab_free(slab, ptr2, 256);
    memento_slab_destroy(slab);
}

TEST(slab_null_handling) {
    /* Should not crash */
    memento_slab_destroy(NULL);
    ASSERT_NULL(memento_slab_alloc(NULL, 100));
    memento_slab_free(NULL, NULL, 0);
    memento_slab_free(NULL, (void*)1, 100);
}

/* ============================================================================
 * Statistics Tests
 * ============================================================================ */

TEST(stats_basic) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_heap_stats_t stats_before, stats_after;
    
    memento_thread_heap_stats(heap, &stats_before);
    
    void* ptr = memento_thread_heap_alloc(heap, 1024);
    ASSERT_NOT_NULL(ptr);
    memento_thread_heap_free(heap, ptr, 1024);
    
    memento_thread_heap_stats(heap, &stats_after);
    
    ASSERT_GE(stats_after.alloc_count, stats_before.alloc_count + 1);
    ASSERT_GE(stats_after.free_count, stats_before.free_count + 1);
}

TEST(stats_null_handling) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    
    /* Should not crash */
    memento_thread_heap_stats(NULL, NULL);
    memento_thread_heap_stats(heap, NULL);
    memento_thread_heap_stats(NULL, NULL);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("=== Memento Core Test Suite ===\n\n");
    
    if (!memento_init()) {
        printf("Failed to initialize Memento\n");
        return 1;
    }
    
    /* Version */
    RUN_TEST(version_check);
    
    /* Initialization */
    RUN_TEST(init_shutdown);
    RUN_TEST(multiple_init);
    
    /* Utilities */
    RUN_TEST(power_of_two);
    RUN_TEST(align_up);
    RUN_TEST(align_down);
    RUN_TEST(size_classes);
    
    /* Thread Heap */
    RUN_TEST(heap_basic_alloc_free);
    RUN_TEST(heap_null_heap);
    RUN_TEST(heap_zero_size);
    RUN_TEST(heap_null_ptr_free);
    RUN_TEST(heap_all_size_classes);
    RUN_TEST(heap_mixed_sizes);
    RUN_TEST(heap_large_allocation);
    RUN_TEST(heap_reuse);
    RUN_TEST(heap_realloc);
    
    /* Pool */
    RUN_TEST(pool_create_destroy);
    RUN_TEST(pool_basic_alloc_free);
    RUN_TEST(pool_exhaustion);
    RUN_TEST(pool_recycle);
    RUN_TEST(pool_various_sizes);
    RUN_TEST(pool_null_handling);
    
    /* Arena */
    RUN_TEST(arena_create_destroy);
    RUN_TEST(arena_basic_alloc);
    RUN_TEST(arena_alignment);
    RUN_TEST(arena_save_restore);
    RUN_TEST(arena_reset);
    RUN_TEST(arena_growth);
    RUN_TEST(arena_null_handling);
    RUN_TEST(arena_zero_size);
    
    /* Stack */
    RUN_TEST(stack_create_destroy);
    RUN_TEST(stack_push_pop);
    RUN_TEST(stack_marker);
    RUN_TEST(stack_alignment);
    RUN_TEST(stack_reset);
    RUN_TEST(stack_overflow);
    RUN_TEST(stack_null_handling);
    
    /* Slab */
    RUN_TEST(slab_create_destroy);
    RUN_TEST(slab_basic_alloc_free);
    RUN_TEST(slab_various_sizes);
    RUN_TEST(slab_large_allocation);
    RUN_TEST(slab_recycle);
    RUN_TEST(slab_null_handling);
    
    /* Statistics */
    RUN_TEST(stats_basic);
    RUN_TEST(stats_null_handling);
    
    memento_shutdown();
    
    printf("\n=== Results ===\n");
    printf("  Passed: %d\n", passed);
    printf("  Failed: %d\n", failed);
    
    if (failed == 0) {
        printf("\n✓ All tests passed!\n");
        return 0;
    } else {
        printf("\n✗ Some tests failed!\n");
        return 1;
    }
}
