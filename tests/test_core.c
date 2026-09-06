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
    ASSERT_EQ(MEMENTO_VERSION_MAJOR, 3);
    ASSERT_EQ(MEMENTO_VERSION_MINOR, 0);
    ASSERT_EQ(MEMENTO_VERSION_PATCH, 0);
    
    /* Check version string */
    ASSERT_NOT_NULL(memento_version_string());
    ASSERT_EQ(strcmp(memento_version_string(), "3.0.0"), 0);
    
    /* Check version number */
    ASSERT_EQ(memento_version_number(), 0x030000);
    
    /* Check version check function */
    ASSERT(memento_version_check(2, 0, 0));
    ASSERT(memento_version_check(1, 99, 99));
    ASSERT(!memento_version_check(4, 0, 0));
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
    /* 0 is not a power of two (would break align_up masks) */
    ASSERT(!memento_is_power_of_two(0));
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
    /* 29 size classes: fine grain in 32–512B, ~1.25x steps in 2K–8K */
    ASSERT_EQ(MEMENTO_SIZE_CLASS_COUNT, 29);
    ASSERT_EQ(memento_size_class_for(1), 0);       /* 1-32 -> class 0 (32B) */
    ASSERT_EQ(memento_size_class_for(32), 0);
    ASSERT_EQ(memento_size_class_for(33), 1);      /* 33-48 -> class 1 (48B) */
    ASSERT_EQ(memento_size_class_for(48), 1);
    ASSERT_EQ(memento_size_class_for(49), 2);      /* 49-64 -> class 2 (64B) */
    ASSERT_EQ(memento_size_class_for(64), 2);
    ASSERT_EQ(memento_size_class_for(80), 3);
    ASSERT_EQ(memento_size_class_for(128), 6);
    ASSERT_EQ(memento_size_class_for(256), 10);
    ASSERT_EQ(memento_size_class_for(512), 14);
    ASSERT_EQ(memento_size_class_for(2048), 21);
    ASSERT_EQ(memento_size_class_for(2560), 22);
    ASSERT_EQ(memento_size_class_for(4096), 24);
    ASSERT_EQ(memento_size_class_for(6144), 26);
    ASSERT_EQ(memento_size_class_for(8192), 28);
    ASSERT_EQ(memento_size_class_for(10000), MEMENTO_SIZE_CLASS_NONE);  /* page-run tier, no class */
    
    /* Test round-trip for every class boundary */
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
    /* malloc(0) contract (both modes agree): a unique, freeable pointer
     * from the smallest class — never NULL, never shared. */
    memento_thread_heap_t* heap = memento_thread_heap_get();
    void* a = memento_thread_heap_alloc(heap, 0);
    void* b = memento_thread_heap_alloc(heap, 0);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_NE(a, b);
    memento_thread_heap_free(heap, a, 0);
    memento_thread_heap_free(heap, b, 0);
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
    
    /* Allocations > 8KB go through large path (+ LOC) */
    size_t large_sizes[] = {8193, 16384, 32768, 65536, 131072, 1048576};
    
    for (size_t i = 0; i < sizeof(large_sizes)/sizeof(large_sizes[0]); i++) {
        void* ptr = memento_thread_heap_alloc(heap, large_sizes[i]);
        ASSERT_NOT_NULL(ptr);
        memset(ptr, 0xEF, large_sizes[i]);
        memento_thread_heap_free(heap, ptr, large_sizes[i]);
    }
}

TEST(heap_large_cache_reuse) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    const size_t sz = 16384;

    void* a = memento_thread_heap_alloc(heap, sz);
    ASSERT_NOT_NULL(a);
    memento_thread_heap_free(heap, a, sz);

    /* Exact-size free should hit the large-object cache */
    void* b = memento_thread_heap_alloc(heap, sz);
    ASSERT_NOT_NULL(b);
    ASSERT_EQ(a, b);
    memento_thread_heap_free(heap, b, sz);
}

TEST(heap_span_refill) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    /* Allocate more than freelist limit to force span refill */
    enum { N = 200 };
    void* ptrs[N];
    for (int i = 0; i < N; i++) {
        ptrs[i] = memento_thread_heap_alloc(heap, 128);
        ASSERT_NOT_NULL(ptrs[i]);
        memset(ptrs[i], (unsigned char)i, 128);
    }
    for (int i = 0; i < N; i++) {
        memento_thread_heap_free(heap, ptrs[i], 128);
    }
    /* Reuse from cache / span freelist */
    for (int i = 0; i < N; i++) {
        ptrs[i] = memento_thread_heap_alloc(heap, 128);
        ASSERT_NOT_NULL(ptrs[i]);
        memento_thread_heap_free(heap, ptrs[i], 128);
    }
}

TEST(heap_numa_node_query) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    ASSERT_NOT_NULL(heap);
    /* -1 if NUMA disabled/unknown; otherwise non-negative node id */
    int node = memento_thread_heap_numa_node(heap);
    ASSERT(node >= -1);
    ASSERT_EQ(memento_thread_heap_numa_node(NULL), -1);
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

TEST(pool_grows) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_pool_t* pool = memento_pool_create(sizeof(int), 10, heap);
    ASSERT_NOT_NULL(pool);

    /* Pools grow by doubling instead of failing: 10 -> 20 -> 40 -> ... */
    void* ptrs[100];
    for (int i = 0; i < 100; i++) {
        ptrs[i] = memento_pool_alloc(pool);
        ASSERT_NOT_NULL(ptrs[i]);
        *(int*)ptrs[i] = i; /* touch */
    }
    /* Writing distinct ints and reading them back catches aliasing. */
    for (int i = 0; i < 100; i++) {
        ASSERT_EQ(*(int*)ptrs[i], i);
    }

    /* Free one and get one back */
    memento_pool_free(pool, ptrs[0]);
    ptrs[0] = memento_pool_alloc(pool);
    ASSERT_NOT_NULL(ptrs[0]);

    for (int i = 0; i < 100; i++) {
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

TEST(arena_guarded_basic) {
    /* Guarded arenas are standalone OS mappings with a PROT_NONE page at
     * the high-water mark. Exercise create/grow/write/destroy; the fault
     * behavior itself is tested in test_lifecycle via fork(). */
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_arena_t* arena = memento_arena_create_guarded(4096, heap);
    ASSERT_NOT_NULL(arena);

    /* Fill the first block completely, then overflow into a second. */
    unsigned char* p = (unsigned char*)memento_arena_alloc(arena, 4096, 16);
    ASSERT_NOT_NULL(p);
    memset(p, 0x5A, 4096);
    void* q = memento_arena_alloc(arena, 2048, 64);
    ASSERT_NOT_NULL(q);
    ASSERT(((uintptr_t)q & 63) == 0);
    memset(q, 0xA5, 2048);

    /* Save/restore within a guarded arena. */
    memento_arena_save_t save = memento_arena_save(arena);
    void* r = memento_arena_alloc(arena, 512, 8);
    ASSERT_NOT_NULL(r);
    memento_arena_restore(arena, &save);

    memento_arena_destroy(arena);
}

TEST(arena_reset_multi_block) {
    /* Grow across several blocks, then reset: excess blocks must go back
     * and the arena must keep serving. */
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_arena_t* arena = memento_arena_create(1024, heap);
    ASSERT_NOT_NULL(arena);

    for (int i = 0; i < 64; i++) {
        void* p = memento_arena_alloc(arena, 512, 16);
        ASSERT_NOT_NULL(p);
        memset(p, i, 512);
    }
    size_t grown = memento_arena_capacity(arena);
    ASSERT(grown > 1024);

    memento_arena_reset(arena);
    ASSERT_EQ(memento_arena_used(arena), 0);
    ASSERT(memento_arena_capacity(arena) < grown);

    void* p = memento_arena_alloc(arena, 100, 8);
    ASSERT_NOT_NULL(p);
    memento_arena_destroy(arena);
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
#if MEMENTO_STATS
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_heap_stats_t stats_before, stats_after;
    
    memento_thread_heap_stats(heap, &stats_before);
    
    void* ptr = memento_thread_heap_alloc(heap, 1024);
    ASSERT_NOT_NULL(ptr);
    memento_thread_heap_free(heap, ptr, 1024);
    
    memento_thread_heap_stats(heap, &stats_after);
    
    ASSERT_GE(stats_after.alloc_count, stats_before.alloc_count + 1);
    ASSERT_GE(stats_after.free_count, stats_before.free_count + 1);
#else
    /* MEMENTO_STATS=0: counters compiled out, must stay zero */
    memento_thread_heap_t* heap = memento_thread_heap_get();
    void* ptr = memento_thread_heap_alloc(heap, 1024);
    ASSERT_NOT_NULL(ptr);
    memento_thread_heap_free(heap, ptr, 1024);
    memento_heap_stats_t st;
    memento_thread_heap_stats(heap, &st);
    ASSERT_EQ(st.alloc_count, 0);
    ASSERT_EQ(st.free_count, 0);
#endif
}

TEST(stats_null_handling) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    
    /* Should not crash */
    memento_thread_heap_stats(NULL, NULL);
    memento_thread_heap_stats(heap, NULL);
    memento_thread_heap_stats(NULL, NULL);
}

/* ============================================================================
 * v3: LUT integrity, aligned alloc, page-run tier, sized basics (exact build)
 * ============================================================================ */

TEST(lut_exhaustive) {
    /* Every size 1..8192 must map to the smallest class that fits it.
     * (v2.2.x shipped a hand-typed LUT whose tail was corrupt: 4096 mapped to
     * the 768-byte class and the allocator happily overflowed the block.
     * This test is the receipt that it stays fixed.) */
    for (size_t s = 1; s <= 8192; s++) {
        size_t sc = memento_size_class_for(s);
        ASSERT(sc < MEMENTO_SIZE_CLASS_COUNT);
        ASSERT(memento_size_class_to_size(sc) >= s);
        if (sc > 0) {
            ASSERT(memento_size_class_to_size(sc - 1) < s);
        }
    }
}

TEST(heap_aligned_alloc_exact) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    size_t aligns[] = {16, 32, 64, 128, 256, 4096};
    for (int i = 0; i < 6; i++) {
        size_t align = aligns[i];
        void* ptr = memento_thread_heap_alloc_aligned(heap, 1000, align);
        ASSERT_NOT_NULL(ptr);
        ASSERT(((uintptr_t)ptr & (align - 1)) == 0);
        memset(ptr, 0xAB, 1000);
        memento_thread_heap_free_aligned(heap, ptr, 1000, align);
    }
    /* SIMD contract: 64-byte aligned buffers for AVX-512 */
    float* vec = (float*)memento_thread_heap_alloc_aligned(heap, 4096, 64);
    ASSERT_NOT_NULL(vec);
    ASSERT(((uintptr_t)vec & 63) == 0);
    memento_thread_heap_free_aligned(heap, vec, 4096, 64);
    /* bad alignment rejected */
    ASSERT_NULL(memento_thread_heap_alloc_aligned(heap, 100, 24));
}

TEST(page_run_tier) {
    memento_thread_heap_t* heap = memento_thread_heap_get();

    /* The 100K JSON buffer scenario: same-size alloc/free cycles must reuse
     * the cached page run instead of donating a fresh VMA each lap. */
    void* first = memento_thread_heap_alloc(heap, 100 * 1024);
    ASSERT_NOT_NULL(first);
    memset(first, 1, 100 * 1024);
    memento_thread_heap_free(heap, first, 100 * 1024);

    int reused = 0;
    for (int i = 0; i < 8; i++) {
        void* p = memento_thread_heap_alloc(heap, 100 * 1024);
        ASSERT_NOT_NULL(p);
        if (p == first) reused++;
        memento_thread_heap_free(heap, p, 100 * 1024);
    }
    ASSERT(reused >= 4); /* cache serves most laps */

    /* Page-run blocks are at least 16-aligned and fully writable */
    void* p = memento_thread_heap_alloc(heap, 200 * 1024);
    ASSERT_NOT_NULL(p);
    ASSERT(((uintptr_t)p & 15) == 0);
    memset(p, 2, 200 * 1024);
    memento_thread_heap_free(heap, p, 200 * 1024);
}

TEST(page_run_realloc_hysteresis) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    /* Grow within the same page count: must not move the block. */
    char* buf = (char*)memento_thread_heap_alloc(heap, 9000);
    ASSERT_NOT_NULL(buf);
    memset(buf, 7, 9000);
    char* grown = (char*)memento_thread_heap_realloc(heap, buf, 9000, 11000);
    ASSERT(grown == buf); /* 9000 and 11000 round to the same 3 pages */
    for (int i = 0; i < 9000; i++) {
        ASSERT(grown[i] == 7);
    }
    memento_thread_heap_free(heap, grown, 11000);
}

TEST(sized_family_in_exact_build) {
    /* The sized API is always available, even in the default exact build. */
    void* p = memento_malloc(123);
    ASSERT_NOT_NULL(p);
    ASSERT_GE(memento_usable_size(p), 123);
    memset(p, 3, 123);
    memento_free(p);

    void* c = memento_calloc(64, 16);
    ASSERT_NOT_NULL(c);
    for (int i = 0; i < 1024; i++) {
        ASSERT(((char*)c)[i] == 0);
    }
    memento_free(c);

    void* a = memento_aligned_alloc(256, 5120); /* C11: size % align == 0 */
    ASSERT_NOT_NULL(a);
    ASSERT(((uintptr_t)a & 255) == 0);
    ASSERT_GE(memento_usable_size(a), 5120);
    memento_free(a);
    ASSERT_NULL(memento_aligned_alloc(256, 5000)); /* C11 reject */

    void* pm = NULL;
    ASSERT_EQ(memento_posix_memalign(&pm, 64, 777), 0);
    ASSERT_NOT_NULL(pm);
    ASSERT(((uintptr_t)pm & 63) == 0);
    memento_free(pm);
    ASSERT(memento_posix_memalign(&pm, 24, 100) != 0); /* not a power of 2 */

    ASSERT_GE(memento_good_size(100), 100);
    ASSERT_EQ(memento_good_size(0), 0);

    memento_free(NULL); /* no-op */
}

TEST(sized_realloc_growth) {
    char* buf = (char*)memento_malloc(100);
    ASSERT_NOT_NULL(buf);
    memset(buf, 9, 100);
    for (int round = 0; round < 10; round++) {
        size_t new_size = 100 << round;
        buf = (char*)memento_realloc(buf, new_size);
        ASSERT_NOT_NULL(buf);
        ASSERT_GE(memento_usable_size(buf), new_size);
        for (int i = 0; i < 100; i++) {
            ASSERT(buf[i] == 9);
        }
    }
    memento_free(buf);
}

TEST(arena_restore_returns_memory) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_arena_t* arena = memento_arena_create(4096, heap);
    ASSERT_NOT_NULL(arena);

    memento_arena_save_t save = memento_arena_save(arena);

    /* Force several growth blocks */
    for (int i = 0; i < 8; i++) {
        void* p = memento_arena_alloc(arena, 64 * 1024, 16);
        ASSERT_NOT_NULL(p);
    }
    size_t capacity_after_growth = memento_arena_capacity(arena);
    ASSERT(capacity_after_growth >= 64 * 1024);

    /* Restore must return the post-save blocks, not just rewind the bump */
    memento_arena_restore(arena, &save);
    ASSERT_EQ(memento_arena_capacity(arena), 4096);
    ASSERT_EQ(memento_arena_used(arena), 0);

    /* And the arena still works afterwards */
    void* p = memento_arena_alloc(arena, 1000, 16);
    ASSERT_NOT_NULL(p);
    memento_arena_destroy(arena);
}

TEST(calloc_zero_always) {
    /* Fresh mappings are kernel-zeroed and calloc skips memset there;
     * recycled blocks must still come back zeroed. Exercise both lives of
     * all three tiers. calloc stamps a sized header in every build, so its
     * pointers go back through the header-routed free, not the exact one. */
    memento_thread_heap_t* heap = memento_thread_heap_get();
    const size_t sizes[3] = { 64, 16384, 300000 };
    for (int t = 0; t < 3; t++) {
        size_t n = sizes[t];
        for (int life = 0; life < 2; life++) {
            unsigned char* p =
                (unsigned char*)memento_heap_calloc(heap, 1, n);
            ASSERT_NOT_NULL(p);
            for (size_t i = 0; i < n; i += 4096 / 8 > n ? 1 : (n / 8 + 1)) {
                ASSERT_EQ(p[i], 0);
            }
            ASSERT_EQ(p[n - 1], 0);
            memset(p, 0x77, n); /* dirty it for the next life */
            memento_heap_mfree(heap, p);
        }
    }
}

TEST(span_empty_reclaim) {
    /* 40k x 64B overflows a single 2 MiB span (which fits ~32k blocks), so
     * the first span retires as a new partial takes over. Freeing the whole
     * burst empties the retired span (live == 0) and parks it. Pages are
     * discarded only after the purge window (MEMENTO_SPAN_PURGE_MS, 10 ms
     * by default) expires — churn inside the window must not pay a single
     * page fault. Sleep past the window, then one more alloc+free cycle
     * triggers a reclaim whose purge walk discards the stale spans. */
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_heap_stats_t before, after;
    memento_thread_heap_stats(heap, &before);

    enum { N = 40000 };
    void** ptrs = (void**)memento_thread_heap_alloc(heap, N * sizeof(void*));
    ASSERT_NOT_NULL(ptrs);
    for (int i = 0; i < N; i++) {
        ptrs[i] = memento_thread_heap_alloc(heap, 64);
        ASSERT_NOT_NULL(ptrs[i]);
        memset(ptrs[i], 0x5A, 64);
    }
    for (int i = 0; i < N; i++) {
        memento_thread_heap_free(heap, ptrs[i], 64);
    }
    memento_thread_heap_free(heap, ptrs, N * sizeof(void*));

    /* Park empty spans (tcache spill + reclaim). Purge is delayed. */
    memento_thread_heap_flush(heap);

#if defined(_WIN32)
    Sleep(50);
#else
    {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 50 * 1000 * 1000;
        nanosleep(&ts, NULL);
    }
#endif

    /* Idle flush after the purge window: discard parked pages. */
    memento_thread_heap_flush(heap);

    memento_thread_heap_stats(heap, &after);
    ASSERT(after.spans_reclaimed > before.spans_reclaimed);

    /* The heap still works after recycling spans (discarded mappings
     * re-fault on touch; freelist blocks keep their old contents — this is
     * malloc, not calloc). */
    void* p = memento_thread_heap_alloc(heap, 64);
    ASSERT_NOT_NULL(p);
    memset(p, 0xA5, 64);
    memento_thread_heap_free(heap, p, 64);
}

TEST(exact_pointer_memento_free_is_sized_only) {
    /* An exact block whose first bytes look like the old 16-bit sized magic
     * must not be consumed by memento_free. After the no-op, exact free still
     * works — the heap did not take a garbage back_off. */
    memento_thread_heap_t* heap = memento_thread_heap_get();
    void* p = memento_thread_heap_alloc(heap, 64);
    ASSERT_NOT_NULL(p);
    memset(p, 0, 64);
    ((uint16_t*)p)[7] = 0x4D53; /* old magic slot if someone sniffed at p+0 */
    /* Also paint the 16 bytes *before* p if they are mapped (previous block
     * tail). memento_free looks at p-16; we only require it not to crash and
     * not to steal p. */
    memento_free(p);
    memset(p, 0xAB, 64);
    memento_thread_heap_free(heap, p, 64);
}

TEST(heap_report_smoke) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    /* NULL heap and NULL stream must not crash */
    memento_thread_heap_report(NULL, NULL);
    /* Real report goes to a temp file so we can sanity-check content */
    FILE* f = tmpfile();
    ASSERT_NOT_NULL(f);
    void* p = memento_thread_heap_alloc(heap, 512);
    memento_thread_heap_report(heap, f);
    memento_thread_heap_free(heap, p, 512);
    fflush(f);
    fseek(f, 0, SEEK_SET);
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    ASSERT(strstr(buf, "memento heap report") != NULL);
    ASSERT(strstr(buf, "foreign frees") != NULL);
    ASSERT(strstr(buf, "bytes live") != NULL);
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
    RUN_TEST(heap_large_cache_reuse);
    RUN_TEST(heap_span_refill);
    RUN_TEST(heap_numa_node_query);
    RUN_TEST(heap_reuse);
    RUN_TEST(heap_realloc);
    
    /* Pool */
    RUN_TEST(pool_create_destroy);
    RUN_TEST(pool_basic_alloc_free);
    RUN_TEST(pool_grows);
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
    RUN_TEST(arena_guarded_basic);
    RUN_TEST(arena_reset_multi_block);
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

    /* v3 additions */
    RUN_TEST(lut_exhaustive);
    RUN_TEST(heap_aligned_alloc_exact);
    RUN_TEST(page_run_tier);
    RUN_TEST(page_run_realloc_hysteresis);
    RUN_TEST(sized_family_in_exact_build);
    RUN_TEST(sized_realloc_growth);
    RUN_TEST(arena_restore_returns_memory);
    RUN_TEST(calloc_zero_always);
    RUN_TEST(span_empty_reclaim);
    RUN_TEST(exact_pointer_memento_free_is_sized_only);
    RUN_TEST(heap_report_smoke);
    
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
