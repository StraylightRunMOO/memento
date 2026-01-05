/*
 * Basic functionality tests for memento allocator
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define MEMENTO_IMPLEMENTATION
#include "../include/memento.h"

static int test_count = 0;
static int pass_count = 0;

#define TEST(name) static void test_##name(void); \
                   static void test_##name(void)

#define RUN_TEST(name) do { \
    printf("  %s... ", #name); \
    fflush(stdout); \
    test_count++; \
    test_##name(); \
    pass_count++; \
    printf("PASS\n"); \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL\n"); \
        printf("    Assertion failed: %s\n", #cond); \
        printf("    at %s:%d\n", __FILE__, __LINE__); \
        exit(1); \
    } \
} while(0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NEQ(a, b) ASSERT((a) != (b))
#define ASSERT_NULL(ptr) ASSERT((ptr) == NULL)
#define ASSERT_NOT_NULL(ptr) ASSERT((ptr) != NULL)

TEST(init_shutdown) {
    /* Ensure clean state */
    if (memento_is_initialized()) {
        memento_shutdown();
    }
    ASSERT(!memento_is_initialized());
    
    ASSERT_EQ(MEMENTO_SUCCESS, memento_init());
    ASSERT(memento_is_initialized());
    ASSERT_NOT_NULL(memento_get_config());
    
    ASSERT_EQ(MEMENTO_SUCCESS, memento_shutdown());
    ASSERT(!memento_is_initialized());
}

TEST(error_strings) {
    ASSERT_NOT_NULL(memento_error_string(MEMENTO_SUCCESS));
    ASSERT_NOT_NULL(memento_error_string(MEMENTO_ERROR_OUT_OF_MEMORY));
    ASSERT_NOT_NULL(memento_error_string(MEMENTO_ERROR_INVALID_ARGUMENT));
}

TEST(alignment_utils) {
    ASSERT_EQ(16, memento_align_up(15, 16));
    ASSERT_EQ(64, memento_align_up(33, 64));
    ASSERT_EQ(128, memento_align_up(128, 128));
    
    ASSERT(memento_is_power_of_two(1));
    ASSERT(memento_is_power_of_two(2));
    ASSERT(memento_is_power_of_two(64));
    ASSERT(!memento_is_power_of_two(3));
    ASSERT(!memento_is_power_of_two(15));
}

TEST(basic_allocation) {
    memento_allocator_t* alloc = memento_create_thread_cache("basic_test");
    ASSERT_NOT_NULL(alloc);
    
    memento_result_t result = memento_alloc(alloc, 1024);
    ASSERT(result.success);
    ASSERT_NOT_NULL(result.ptr);
    ASSERT_EQ(1024, result.size);
    
    memset(result.ptr, 0xAB, 1024);
    memento_free(alloc, result.ptr);
    
    memento_destroy_allocator(alloc);
}

TEST(zero_size_allocation) {
    memento_allocator_t* alloc = memento_create_thread_cache("zero_test");
    ASSERT_NOT_NULL(alloc);
    
    memento_result_t result = memento_alloc(alloc, 0);
    ASSERT(result.success);
    ASSERT_NULL(result.ptr);
    ASSERT_EQ(0, result.size);
    
    memento_destroy_allocator(alloc);
}

TEST(aligned_allocation) {
    memento_allocator_t* alloc = memento_create_thread_cache("aligned_test");
    ASSERT_NOT_NULL(alloc);
    
    memento_result_t result = memento_alloc_aligned(alloc, 1024, 64);
    ASSERT(result.success);
    ASSERT_NOT_NULL(result.ptr);
    
    uintptr_t ptr_val = (uintptr_t)result.ptr;
    ASSERT_EQ(0, ptr_val % 64);
    
    memento_free(alloc, result.ptr);
    memento_destroy_allocator(alloc);
}

TEST(statistics_tracking) {
    memento_allocator_t* alloc = memento_create_thread_cache("stats_test");
    ASSERT_NOT_NULL(alloc);
    
    const memento_stats_t* stats = memento_get_stats(alloc);
    ASSERT_NOT_NULL(stats);
    
    size_t initial_allocs = stats->allocation_count;
    
    void* ptr1 = memento_alloc(alloc, 100).ptr;
    void* ptr2 = memento_alloc(alloc, 200).ptr;
    
    stats = memento_get_stats(alloc);
    ASSERT_EQ(initial_allocs + 2, stats->allocation_count);
    
    memento_free(alloc, ptr1);
    memento_free(alloc, ptr2);
    
    memento_destroy_allocator(alloc);
}

TEST(multiple_allocators) {
    memento_allocator_t* alloc1 = memento_create_thread_cache("multi1");
    memento_allocator_t* alloc2 = memento_create_thread_cache("multi2");
    
    ASSERT_NOT_NULL(alloc1);
    ASSERT_NOT_NULL(alloc2);
    ASSERT_NEQ(alloc1, alloc2);
    
    void* ptr1 = memento_alloc(alloc1, 512).ptr;
    void* ptr2 = memento_alloc(alloc2, 1024).ptr;
    
    ASSERT_NOT_NULL(ptr1);
    ASSERT_NOT_NULL(ptr2);
    
    memento_free(alloc1, ptr1);
    memento_free(alloc2, ptr2);
    
    memento_destroy_allocator(alloc1);
    memento_destroy_allocator(alloc2);
}

int main(void) {
    printf("Memento Basic Tests\n");
    printf("===================\n");
    
    if (memento_init() != MEMENTO_SUCCESS) {
        printf("Failed to initialize memento\n");
        return 1;
    }
    
    RUN_TEST(init_shutdown);
    RUN_TEST(error_strings);
    RUN_TEST(alignment_utils);
    RUN_TEST(basic_allocation);
    RUN_TEST(zero_size_allocation);
    RUN_TEST(aligned_allocation);
    RUN_TEST(statistics_tracking);
    RUN_TEST(multiple_allocators);
    
    memento_shutdown();
    
    printf("\n========================================\n");
    printf("Test Summary:\n");
    printf("  Total: %d\n", test_count);
    printf("  Passed: %d\n", pass_count);
    printf("  Failed: %d\n", test_count - pass_count);
    printf("========================================\n");
    
    return (test_count == pass_count) ? 0 : 1;
}
