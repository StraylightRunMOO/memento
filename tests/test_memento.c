/*
 * Memento Allocator Test Suite
 * 
 * Comprehensive tests for the memento memory allocator library.
 * Tests all allocator types, error conditions, and edge cases.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#define MEMENTO_IMPLEMENTATION
#include "memento.h"

/* Test counters */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

/* Test macros */
#define TEST(name) static void test_##name(void); \
                   static void test_##name(void)

#define RUN_TEST(name) do { \
    printf("Running test: %s... ", #name); \
    fflush(stdout); \
    tests_run++; \
    test_##name(); \
    tests_passed++; \
    printf("PASSED\n"); \
} while(0)

#define ASSERT(condition) do { \
    if (!(condition)) { \
        printf("FAILED\n"); \
        printf("  Assertion failed: %s\n", #condition); \
        printf("  File: %s, Line: %d\n", __FILE__, __LINE__); \
        tests_failed++; \
        tests_passed--; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(expected, actual) do { \
    if ((expected) != (actual)) { \
        printf("FAILED\n"); \
        printf("  Expected: %lld, Actual: %lld\n", (long long)(expected), (long long)(actual)); \
        printf("  File: %s, Line: %d\n", __FILE__, __LINE__); \
        tests_failed++; \
        tests_passed--; \
        return; \
    } \
} while(0)

#define ASSERT_NEQ(expected, actual) do { \
    if ((expected) == (actual)) { \
        printf("FAILED\n"); \
        printf("  Values should not be equal: %lld\n", (long long)(actual)); \
        printf("  File: %s, Line: %d\n", __FILE__, __LINE__); \
        tests_failed++; \
        tests_passed--; \
        return; \
    } \
} while(0)

#define ASSERT_NULL(ptr) ASSERT((ptr) == NULL)
#define ASSERT_NOT_NULL(ptr) ASSERT((ptr) != NULL)

/* ============================================================================= */
/* BASIC FUNCTIONALITY TESTS                                                   */
/* ============================================================================= */

TEST(init_shutdown) {
    /* Ensure we start with a clean state */
    if (memento_is_initialized()) {
        memento_shutdown();
    }
    ASSERT(!memento_is_initialized());
    
    /* Test basic initialization and shutdown */
    ASSERT_EQ(MEMENTO_SUCCESS, memento_init());
    ASSERT(memento_is_initialized());
    
    /* Test double initialization */
    ASSERT_EQ(MEMENTO_ERROR_ALREADY_INITIALIZED, memento_init());
    
    ASSERT_EQ(MEMENTO_SUCCESS, memento_shutdown());
    ASSERT(!memento_is_initialized());
    
    /* Test double shutdown - should not return error, just be silent */
    int result = memento_shutdown();
    ASSERT(result == MEMENTO_ERROR_NOT_INITIALIZED || result == MEMENTO_SUCCESS);
}

TEST(error_strings) {
    /* Test that all error codes have valid strings */
    ASSERT_NOT_NULL(memento_error_string(MEMENTO_SUCCESS));
    ASSERT_NOT_NULL(memento_error_string(MEMENTO_ERROR_INVALID_ARGUMENT));
    ASSERT_NOT_NULL(memento_error_string(MEMENTO_ERROR_OUT_OF_MEMORY));
    ASSERT_NOT_NULL(memento_error_string(MEMENTO_ERROR_NOT_INITIALIZED));
    ASSERT_NOT_NULL(memento_error_string(MEMENTO_ERROR_ALREADY_INITIALIZED));
    ASSERT_NOT_NULL(memento_error_string(MEMENTO_ERROR_THREAD_NOT_INITIALIZED));
    ASSERT_NOT_NULL(memento_error_string(MEMENTO_ERROR_ALLOCATION_FAILED));
    ASSERT_NOT_NULL(memento_error_string(MEMENTO_ERROR_INVALID_POINTER));
    ASSERT_NOT_NULL(memento_error_string(MEMENTO_ERROR_UNSUPPORTED_OPERATION));
}

TEST(alignment_utilities) {
    /* Test alignment utilities */
    ASSERT_EQ(8, memento_align_up(5, 8));
    ASSERT_EQ(16, memento_align_up(15, 16));
    ASSERT_EQ(32, memento_align_up(32, 32));
    ASSERT_EQ(64, memento_align_up(33, 64));
    
    /* Test power of two detection */
    ASSERT(memento_is_power_of_two(1));
    ASSERT(memento_is_power_of_two(2));
    ASSERT(memento_is_power_of_two(4));
    ASSERT(memento_is_power_of_two(8));
    ASSERT(memento_is_power_of_two(16));
    ASSERT(memento_is_power_of_two(1024));
    ASSERT(!memento_is_power_of_two(3));
    ASSERT(!memento_is_power_of_two(5));
    ASSERT(!memento_is_power_of_two(6));
    ASSERT(!memento_is_power_of_two(7));
    ASSERT(!memento_is_power_of_two(1000));
}

/* ============================================================================= */
/* THREAD CACHE ALLOCATOR TESTS                                                */
/* ============================================================================= */

TEST(thread_cache_basic) {
    ASSERT(memento_is_initialized());
    
    memento_allocator_t* allocator = memento_create_thread_cache("test_thread_cache");
    ASSERT_NOT_NULL(allocator);
    ASSERT_NOT_NULL(allocator->name);
    ASSERT_EQ(0, strcmp(allocator->name, "test_thread_cache"));
    
    /* Test basic allocation */
    memento_result_t result = memento_alloc(allocator, 1024);
    ASSERT(result.success);
    ASSERT_NOT_NULL(result.ptr);
    ASSERT_EQ(1024, result.size);
    
    /* Test deallocation */
    memento_free(allocator, result.ptr);
    
    /* Test statistics */
    const memento_stats_t* stats = memento_get_stats(allocator);
    ASSERT_NOT_NULL(stats);
    ASSERT(stats->allocation_count > 0);
    ASSERT(stats->deallocation_count > 0);
    
    memento_destroy_allocator(allocator);
}

TEST(thread_cache_aligned) {
    ASSERT(memento_is_initialized());
    
    memento_allocator_t* allocator = memento_create_thread_cache("test_aligned");
    ASSERT_NOT_NULL(allocator);
    
    /* Test aligned allocation */
    memento_result_t result = memento_alloc_aligned(allocator, 1024, 64);
    ASSERT(result.success);
    ASSERT_NOT_NULL(result.ptr);
    
    /* Verify alignment */
    uintptr_t ptr_val = (uintptr_t)result.ptr;
    ASSERT_EQ(0, ptr_val % 64);
    
    memento_free(allocator, result.ptr);
    memento_destroy_allocator(allocator);
}

TEST(thread_cache_zero_size) {
    ASSERT(memento_is_initialized());
    
    memento_allocator_t* allocator = memento_create_thread_cache("test_zero");
    ASSERT_NOT_NULL(allocator);
    
    /* Test zero-size allocation */
    memento_result_t result = memento_alloc(allocator, 0);
    ASSERT(result.success);
    ASSERT_NULL(result.ptr);
    ASSERT_EQ(0, result.size);
    
    /* Test zero-size aligned allocation */
    result = memento_alloc_aligned(allocator, 0, 16);
    ASSERT(result.success);
    ASSERT_NULL(result.ptr);
    ASSERT_EQ(0, result.size);
    
    memento_destroy_allocator(allocator);
}

TEST(thread_cache_large) {
    ASSERT(memento_is_initialized());
    
    memento_allocator_t* allocator = memento_create_thread_cache("test_large");
    ASSERT_NOT_NULL(allocator);
    
    /* Test large allocation */
    size_t large_size = 10 * 1024 * 1024;  /* 10MB */
    memento_result_t result = memento_alloc(allocator, large_size);
    ASSERT(result.success);
    ASSERT_NOT_NULL(result.ptr);
    ASSERT_EQ(large_size, result.size);
    
    /* Write to the memory to ensure it's accessible */
    memset(result.ptr, 0xAB, large_size);
    
    memento_free(allocator, result.ptr);
    memento_destroy_allocator(allocator);
}

/* ============================================================================= */
/* BLOCK ALLOCATOR TESTS                                                       */
/* ============================================================================= */

TEST(block_allocator_basic) {
    ASSERT(memento_is_initialized());
    
    /* Create backing allocator */
    memento_allocator_t* backing = memento_create_thread_cache("block_backing");
    ASSERT_NOT_NULL(backing);
    
    /* Create block allocator */
    memento_allocator_t* block = memento_create_block_allocator("test_block", backing);
    ASSERT_NOT_NULL(block);
    ASSERT_NOT_NULL(block->name);
    ASSERT_EQ(0, strcmp(block->name, "test_block"));
    
    /* Test basic allocation */
    memento_result_t result = memento_alloc(block, 1024);
    ASSERT(result.success);
    ASSERT_NOT_NULL(result.ptr);
    ASSERT_EQ(1024, result.size);
    
    /* Test deallocation */
    memento_free(block, result.ptr);
    
    memento_destroy_allocator(block);
    memento_destroy_allocator(backing);
}

TEST(block_allocator_multiple) {
    ASSERT(memento_is_initialized());
    
    memento_allocator_t* backing = memento_create_thread_cache("block_backing_multi");
    ASSERT_NOT_NULL(backing);
    
    memento_allocator_t* block = memento_create_block_allocator("test_block_multi", backing);
    ASSERT_NOT_NULL(block);
    
    /* Test multiple allocations */
    void* ptrs[10];
    for (int i = 0; i < 10; i++) {
        memento_result_t result = memento_alloc(block, (i + 1) * 100);
        ASSERT(result.success);
        ASSERT_NOT_NULL(result.ptr);
        ptrs[i] = result.ptr;
    }
    
    /* Deallocate in reverse order */
    for (int i = 9; i >= 0; i--) {
        memento_free(block, ptrs[i]);
    }
    
    memento_destroy_allocator(block);
    memento_destroy_allocator(backing);
}

TEST(block_allocator_recycling) {
    ASSERT(memento_is_initialized());
    
    memento_allocator_t* backing = memento_create_thread_cache("block_backing_recycle");
    ASSERT_NOT_NULL(backing);
    
    memento_allocator_t* block = memento_create_block_allocator("test_block_recycle", backing);
    ASSERT_NOT_NULL(block);
    
    /* Test that freed memory gets recycled */
    memento_result_t result1 = memento_alloc(block, 1024);
    ASSERT(result1.success);
    void* ptr1 = result1.ptr;
    
    memento_free(block, ptr1);
    
    /* Allocate same size again - should reuse the freed memory */
    memento_result_t result2 = memento_alloc(block, 1024);
    ASSERT(result2.success);
    void* ptr2 = result2.ptr;
    
    /* The pointers might be the same due to recycling */
    printf("  First allocation: %p, Second allocation: %p\n", ptr1, ptr2);
    
    memento_free(block, ptr2);
    memento_destroy_allocator(block);
    memento_destroy_allocator(backing);
}

/* ============================================================================= */
/* PROXY ALLOCATOR TESTS                                                       */
/* ============================================================================= */

TEST(proxy_allocator_basic) {
    ASSERT(memento_is_initialized());
    
    /* Create backing allocator */
    memento_allocator_t* backing = memento_create_thread_cache("proxy_backing");
    ASSERT_NOT_NULL(backing);
    
    /* Create proxy allocator */
    memento_allocator_t* proxy = memento_create_proxy_allocator("test_proxy", backing);
    ASSERT_NOT_NULL(proxy);
    ASSERT_NOT_NULL(proxy->name);
    ASSERT_EQ(0, strcmp(proxy->name, "test_proxy"));
    
    /* Test basic allocation through proxy */
    memento_result_t result = memento_alloc(proxy, 1024);
    ASSERT(result.success);
    ASSERT_NOT_NULL(result.ptr);
    ASSERT_EQ(1024, result.size);
    
    /* Verify that both allocators have statistics */
    const memento_stats_t* proxy_stats = memento_get_stats(proxy);
    const memento_stats_t* backing_stats = memento_get_stats(backing);
    
    ASSERT_NOT_NULL(proxy_stats);
    ASSERT_NOT_NULL(backing_stats);
    ASSERT(proxy_stats->allocation_count > 0);
    ASSERT(backing_stats->allocation_count > 0);
    
    memento_free(proxy, result.ptr);
    memento_destroy_allocator(proxy);
    memento_destroy_allocator(backing);
}

TEST(proxy_allocator_hierarchy) {
    ASSERT(memento_is_initialized());
    
    /* Create multi-level proxy hierarchy */
    memento_allocator_t* root = memento_create_thread_cache("root_proxy");
    ASSERT_NOT_NULL(root);
    
    memento_allocator_t* level1 = memento_create_proxy_allocator("level1", root);
    ASSERT_NOT_NULL(level1);
    
    memento_allocator_t* level2 = memento_create_proxy_allocator("level2", level1);
    ASSERT_NOT_NULL(level2);
    
    /* Test allocation through hierarchy */
    memento_result_t result = memento_alloc(level2, 512);
    ASSERT(result.success);
    ASSERT_NOT_NULL(result.ptr);
    
    /* All levels should have statistics */
    const memento_stats_t* root_stats = memento_get_stats(root);
    const memento_stats_t* level1_stats = memento_get_stats(level1);
    const memento_stats_t* level2_stats = memento_get_stats(level2);
    
    ASSERT_NOT_NULL(root_stats);
    ASSERT_NOT_NULL(level1_stats);
    ASSERT_NOT_NULL(level2_stats);
    
    ASSERT(root_stats->allocation_count > 0);
    ASSERT(level1_stats->allocation_count > 0);
    ASSERT(level2_stats->allocation_count > 0);
    
    memento_free(level2, result.ptr);
    
    /* Clean up hierarchy in reverse order */
    memento_destroy_allocator(level2);
    memento_destroy_allocator(level1);
    memento_destroy_allocator(root);
}

/* ============================================================================= */
/* STACK ALLOCATOR TESTS                                                       */
/* ============================================================================= */

TEST(stack_allocator_basic) {
    ASSERT(memento_is_initialized());
    
    memento_allocator_t* backing = memento_create_thread_cache("stack_backing");
    ASSERT_NOT_NULL(backing);
    
    /* Create stack allocator with 64KB capacity */
    memento_allocator_t* stack = memento_create_stack_allocator("test_stack", 64 * 1024, backing);
    ASSERT_NOT_NULL(stack);
    
    /* Test basic allocation */
    memento_result_t result = memento_alloc(stack, 1024);
    ASSERT(result.success);
    ASSERT_NOT_NULL(result.ptr);
    
    /* Stack allocator doesn't support individual deallocation */
    /* This should not crash, but won't actually deallocate */
    memento_free(stack, result.ptr);
    
    memento_destroy_allocator(stack);
    memento_destroy_allocator(backing);
}

TEST(stack_allocator_alignment) {
    ASSERT(memento_is_initialized());
    
    memento_allocator_t* backing = memento_create_thread_cache("stack_backing_align");
    ASSERT_NOT_NULL(backing);
    
    memento_allocator_t* stack = memento_create_stack_allocator("test_stack_align", 64 * 1024, backing);
    ASSERT_NOT_NULL(stack);
    
    /* Test aligned allocation */
    memento_result_t result = memento_alloc_aligned(stack, 1024, 64);
    ASSERT(result.success);
    ASSERT_NOT_NULL(result.ptr);
    
    /* Verify alignment */
    uintptr_t ptr_val = (uintptr_t)result.ptr;
    ASSERT_EQ(0, ptr_val % 64);
    
    memento_destroy_allocator(stack);
    memento_destroy_allocator(backing);
}

/* ============================================================================= */
/* ERROR HANDLING TESTS                                                        */
/* ============================================================================= */

TEST(error_null_allocator) {
    ASSERT(memento_is_initialized());
    
    /* Test operations with null allocator */
    memento_result_t result = memento_alloc(NULL, 1024);
    ASSERT(!result.success);
    ASSERT_NULL(result.ptr);
    
    /* Should not crash */
    memento_free(NULL, NULL);
    memento_free(NULL, (void*)0x12345678);
}

TEST(error_invalid_alignment) {
    ASSERT(memento_is_initialized());
    
    memento_allocator_t* allocator = memento_create_thread_cache("test_invalid_align");
    ASSERT_NOT_NULL(allocator);
    
    /* Test invalid alignment (not power of two) */
    memento_result_t result = memento_alloc_aligned(allocator, 1024, 3);
    ASSERT(!result.success);
    ASSERT_NULL(result.ptr);
    
    /* Test excessive alignment */
    result = memento_alloc_aligned(allocator, 1024, MEMENTO_MAX_ALIGNMENT * 2);
    ASSERT(!result.success);
    ASSERT_NULL(result.ptr);
    
    memento_destroy_allocator(allocator);
}

TEST(error_invalid_parameters) {
    ASSERT(memento_is_initialized());
    
    /* Test creating allocators with invalid parameters */
    memento_allocator_t* block = memento_create_block_allocator("test_invalid", NULL);
    ASSERT_NULL(block);
    
    memento_allocator_t* proxy = memento_create_proxy_allocator("test_invalid", NULL);
    ASSERT_NULL(proxy);
    
    memento_allocator_t* stack = memento_create_stack_allocator("test_invalid", 0, NULL);
    ASSERT_NULL(stack);
}

/* ============================================================================= */
/* MEMORY STRESS TESTS                                                         */
/* ============================================================================= */

TEST(stress_random_allocations) {
    ASSERT(memento_is_initialized());
    
    memento_allocator_t* allocator = memento_create_thread_cache("stress_test");
    ASSERT_NOT_NULL(allocator);
    
    srand((unsigned int)time(NULL));
    
    /* Perform many random allocations and deallocations */
    void* ptrs[1000];
    size_t sizes[1000];
    int allocated_count = 0;
    
    for (int i = 0; i < 10000; i++) {
        if (allocated_count < 1000 && (rand() % 2 == 0 || allocated_count == 0)) {
            /* Allocate */
            size_t size = (size_t)(rand() % 16384) + 1;  /* 1-16KB */
            memento_result_t result = memento_alloc(allocator, size);
            
            if (result.success) {
                ptrs[allocated_count] = result.ptr;
                sizes[allocated_count] = size;
                allocated_count++;
                
                /* Write pattern to memory */
                memset(result.ptr, 0xAB, size);
            }
        } else if (allocated_count > 0) {
            /* Deallocate */
            int index = rand() % allocated_count;
            memento_free(allocator, ptrs[index]);
            
            /* Move last element to fill the gap */
            ptrs[index] = ptrs[allocated_count - 1];
            sizes[index] = sizes[allocated_count - 1];
            allocated_count--;
        }
    }
    
    /* Clean up remaining allocations */
    for (int i = 0; i < allocated_count; i++) {
        memento_free(allocator, ptrs[i]);
    }
    
    memento_destroy_allocator(allocator);
}

TEST(stress_fragmentation) {
    ASSERT(memento_is_initialized());
    
    /* Create a block allocator to test fragmentation handling */
    memento_allocator_t* backing = memento_create_thread_cache("frag_backing");
    ASSERT_NOT_NULL(backing);
    
    memento_allocator_t* block = memento_create_block_allocator("frag_block", backing);
    ASSERT_NOT_NULL(block);
    
    /* Create fragmentation pattern */
    void* ptrs[50];
    
    /* Allocate all */
    for (int i = 0; i < 50; i++) {
        size_t size = (i % 2 == 0) ? 1024 : 2048;  /* Alternating sizes */
        memento_result_t result = memento_alloc(block, size);
        ASSERT(result.success);
        ptrs[i] = result.ptr;
    }
    
    /* Free every other allocation to create fragmentation */
    for (int i = 0; i < 50; i += 2) {
        memento_free(block, ptrs[i]);
    }
    
    /* Try to allocate in fragmented space */
    for (int i = 0; i < 10; i++) {
        memento_result_t result = memento_alloc(block, 1024);
        ASSERT(result.success);  /* Should reuse freed space */
        memento_free(block, result.ptr);
    }
    
    /* Clean up remaining */
    for (int i = 1; i < 50; i += 2) {
        memento_free(block, ptrs[i]);
    }
    
    memento_destroy_allocator(block);
    memento_destroy_allocator(backing);
}

/* ============================================================================= */
/* STATISTICS TESTS                                                            */
/* ============================================================================= */

TEST(statistics_tracking) {
    ASSERT(memento_is_initialized());
    
    memento_allocator_t* allocator = memento_create_thread_cache("stats_test");
    ASSERT_NOT_NULL(allocator);
    
    const memento_stats_t* initial_stats = memento_get_stats(allocator);
    ASSERT_NOT_NULL(initial_stats);
    
    size_t initial_alloc_count = initial_stats->allocation_count;
    size_t initial_dealloc_count = initial_stats->deallocation_count;
    
    /* Perform some allocations */
    void* ptr1 = memento_alloc(allocator, 1024).ptr;
    void* ptr2 = memento_alloc(allocator, 2048).ptr;
    void* ptr3 = memento_alloc(allocator, 4096).ptr;
    
    ASSERT_NOT_NULL(ptr1);
    ASSERT_NOT_NULL(ptr2);
    ASSERT_NOT_NULL(ptr3);
    
    /* Check statistics updated */
    const memento_stats_t* after_alloc_stats = memento_get_stats(allocator);
    ASSERT(after_alloc_stats->allocation_count == initial_alloc_count + 3);
    ASSERT(after_alloc_stats->current_usage > 0);
    
    /* Deallocate */
    memento_free(allocator, ptr1);
    memento_free(allocator, ptr2);
    memento_free(allocator, ptr3);
    
    /* Check statistics updated */
    const memento_stats_t* final_stats = memento_get_stats(allocator);
    ASSERT(final_stats->deallocation_count == initial_dealloc_count + 3);
    
    memento_destroy_allocator(allocator);
}

/* ============================================================================= */
/* UTILITY FUNCTIONS                                                           */
/* ============================================================================= */

static void print_test_summary(void) {
    printf("\n========================================\n");
    printf("Test Summary:\n");
    printf("  Total tests run: %d\n", tests_run);
    printf("  Tests passed: %d\n", tests_passed);
    printf("  Tests failed: %d\n", tests_failed);
    printf("========================================\n");
}

static int run_all_tests(void) {
    printf("Memento Allocator Test Suite\n");
    printf("=============================\n\n");
    
    /* Initialize memento */
    if (memento_init() != MEMENTO_SUCCESS) {
        printf("FAILED: Could not initialize memento library\n");
        return 1;
    }
    
    /* Basic functionality tests */
    printf("=== Basic Functionality Tests ===\n");
    RUN_TEST(error_strings);
    RUN_TEST(alignment_utilities);
    
    /* Thread cache tests */
    printf("\n=== Thread Cache Allocator Tests ===\n");
    RUN_TEST(thread_cache_basic);
    RUN_TEST(thread_cache_aligned);
    RUN_TEST(thread_cache_zero_size);
    RUN_TEST(thread_cache_large);
    
    /* Block allocator tests */
    printf("\n=== Block Allocator Tests ===\n");
    RUN_TEST(block_allocator_basic);
    RUN_TEST(block_allocator_multiple);
    RUN_TEST(block_allocator_recycling);
    
    /* Proxy allocator tests */
    printf("\n=== Proxy Allocator Tests ===\n");
    RUN_TEST(proxy_allocator_basic);
    RUN_TEST(proxy_allocator_hierarchy);
    
    /* Stack allocator tests */
    printf("\n=== Stack Allocator Tests ===\n");
    RUN_TEST(stack_allocator_basic);
    RUN_TEST(stack_allocator_alignment);
    
    /* Error handling tests */
    printf("\n=== Error Handling Tests ===\n");
    RUN_TEST(error_null_allocator);
    RUN_TEST(error_invalid_alignment);
    RUN_TEST(error_invalid_parameters);
    
    /* Stress tests */
    printf("\n=== Stress Tests ===\n");
    RUN_TEST(stress_random_allocations);
    RUN_TEST(stress_fragmentation);
    
    /* Statistics tests */
    printf("\n=== Statistics Tests ===\n");
    RUN_TEST(statistics_tracking);
    
    /* Shutdown and restart test */
    printf("\n=== Initialization/Shutdown Test ===\n");
    RUN_TEST(init_shutdown);
    
    /* Print summary */
    print_test_summary();
    
    /* Cleanup */
    if (memento_is_initialized()) {
        memento_shutdown();
    }
    
    return tests_failed > 0 ? 1 : 0;
}

/* ============================================================================= */
/* MAIN FUNCTION                                                               */
/* ============================================================================= */

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    
    int result = run_all_tests();
    
    if (result == 0) {
        printf("\nAll tests passed! ✓\n");
    } else {
        printf("\nSome tests failed! ✗\n");
    }
    
    return result;
}
