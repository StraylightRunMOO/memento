/*
 * Memory leak detection tests for memento allocator
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

#define ASSERT_NOT_NULL(ptr) ASSERT((ptr) != NULL)
#define ASSERT_NULL(ptr) ASSERT((ptr) == NULL)

TEST(basic_leak_detection) {
    memento_allocator_t* alloc = memento_create_thread_cache("leak_test");
    ASSERT_NOT_NULL(alloc);
    
    const memento_stats_t* initial_stats = memento_get_stats(alloc);
    size_t initial_allocs = initial_stats->allocation_count;
    
    /* Allocate without freeing */
    void* ptr1 = memento_alloc(alloc, 100).ptr;
    void* ptr2 = memento_alloc(alloc, 200).ptr;
    void* ptr3 = memento_alloc(alloc, 300).ptr;
    
    ASSERT_NOT_NULL(ptr1);
    ASSERT_NOT_NULL(ptr2);
    ASSERT_NOT_NULL(ptr3);
    
    /* Check that allocations are tracked */
    const memento_stats_t* after_stats = memento_get_stats(alloc);
    ASSERT(after_stats->allocation_count == initial_allocs + 3);
    
    /* Free the memory */
    memento_free(alloc, ptr1);
    memento_free(alloc, ptr2);
    memento_free(alloc, ptr3);
    
    memento_destroy_allocator(alloc);
}

TEST(proxy_leak_tracking) {
    memento_allocator_t* backing = memento_create_thread_cache("proxy_leak_back");
    ASSERT_NOT_NULL(backing);
    
    /* Get initial stats */
    /* const memento_stats_t* initial_backing_stats = memento_get_stats(backing); // For future leak detection */
    
    memento_allocator_t* proxy = memento_create_proxy_allocator("proxy_leak", backing);
    ASSERT_NOT_NULL(proxy);
    
    /* Proxy creation may have allocated memory for internal structures */
    const memento_stats_t* after_create_backing_stats = memento_get_stats(backing);
    size_t after_create_backing_allocs = after_create_backing_stats->allocation_count;
    
    const memento_stats_t* proxy_stats = memento_get_stats(proxy);
    size_t initial_proxy_allocs = proxy_stats->allocation_count;
    
    /* Allocate through proxy */
    void* ptr = memento_alloc(proxy, 1024).ptr;
    ASSERT_NOT_NULL(ptr);
    
    /* Both proxy and backing should track the allocation */
    proxy_stats = memento_get_stats(proxy);
    const memento_stats_t* final_backing_stats = memento_get_stats(backing);
    
    /* Proxy should have at least one more allocation (the user allocation) */
    ASSERT(proxy_stats->allocation_count >= initial_proxy_allocs + 1);
    
    /* Backing should have at least one more allocation than after proxy creation */
    ASSERT(final_backing_stats->allocation_count >= after_create_backing_allocs + 1);
    
    memento_free(proxy, ptr);
    
    memento_destroy_allocator(proxy);
    memento_destroy_allocator(backing);
}

TEST(hierarchical_leak_detection) {
    memento_allocator_t* root = memento_create_thread_cache("hier_leak_root");
    memento_allocator_t* level1 = memento_create_proxy_allocator("hier_leak_l1", root);
    memento_allocator_t* level2 = memento_create_proxy_allocator("hier_leak_l2", level1);
    
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(level1);
    ASSERT_NOT_NULL(level2);
    
    /* Allocate through hierarchy */
    void* ptr = memento_alloc(level2, 512).ptr;
    ASSERT_NOT_NULL(ptr);
    
    /* All levels should track the allocation */
    const memento_stats_t* root_stats = memento_get_stats(root);
    const memento_stats_t* l1_stats = memento_get_stats(level1);
    const memento_stats_t* l2_stats = memento_get_stats(level2);
    
    ASSERT(root_stats->allocation_count > 0);
    ASSERT(l1_stats->allocation_count > 0);
    ASSERT(l2_stats->allocation_count > 0);
    
    memento_free(level2, ptr);
    
    memento_destroy_allocator(level2);
    memento_destroy_allocator(level1);
    memento_destroy_allocator(root);
}

TEST(statistics_integrity) {
    memento_allocator_t* alloc = memento_create_thread_cache("stats_integrity");
    ASSERT_NOT_NULL(alloc);
    
    const memento_stats_t* stats = memento_get_stats(alloc);
    size_t initial_allocs = stats->allocation_count;
    size_t initial_deallocs = stats->deallocation_count;
    
    /* Perform balanced allocations and deallocations */
    for (int i = 0; i < 10; i++) {
        void* ptr = memento_alloc(alloc, 100).ptr;
        ASSERT_NOT_NULL(ptr);
        memento_free(alloc, ptr);
    }
    
    stats = memento_get_stats(alloc);
    ASSERT(stats->allocation_count == initial_allocs + 10);
    ASSERT(stats->deallocation_count == initial_deallocs + 10);
    
    memento_destroy_allocator(alloc);
}

TEST(failed_allocation_tracking) {
    memento_allocator_t* alloc = memento_create_thread_cache("fail_track");
    ASSERT_NOT_NULL(alloc);
    
    const memento_stats_t* stats = memento_get_stats(alloc);
    size_t initial_failed = stats->failed_allocations;
    
    /* Test with invalid alignment - this should fail but not count as failed allocation */
    memento_result_t result = memento_alloc_aligned(alloc, 1024, 3);  /* Not power of two */
    ASSERT(!result.success);
    ASSERT_NULL(result.ptr);
    
    /* Invalid parameters don't count as failed allocations - they're rejected early */
    stats = memento_get_stats(alloc);
    ASSERT(stats->failed_allocations == initial_failed);
    
    /* Test with excessive alignment - this should also fail */
    result = memento_alloc_aligned(alloc, 1024, MEMENTO_MAX_ALIGNMENT * 2);
    ASSERT(!result.success);
    ASSERT_NULL(result.ptr);
    
    stats = memento_get_stats(alloc);
    ASSERT(stats->failed_allocations == initial_failed);
    
    /* Test a real failed allocation - try to allocate more memory than available */
    result = memento_alloc(alloc, SIZE_MAX);
    if (!result.success) {
        /* This should count as a failed allocation */
        stats = memento_get_stats(alloc);
        ASSERT(stats->failed_allocations == initial_failed + 1);
    } else {
        /* If it succeeded, it's not a failed allocation */
        printf("    Large allocation succeeded (virtual memory)\n");
        memento_free(alloc, result.ptr);
    }
    
    memento_destroy_allocator(alloc);
}

TEST(memory_corruption_detection) {
    memento_allocator_t* alloc = memento_create_thread_cache("corruption_test");
    ASSERT_NOT_NULL(alloc);
    
    /* Allocate and write pattern */
    size_t size = 1024;
    memento_result_t result = memento_alloc(alloc, size);
    ASSERT(result.success);
    ASSERT_NOT_NULL(result.ptr);
    
    unsigned char* bytes = (unsigned char*)result.ptr;
    
    /* Write pattern */
    for (size_t i = 0; i < size; i++) {
        bytes[i] = (unsigned char)(i & 0xFF);
    }
    
    /* Verify pattern */
    for (size_t i = 0; i < size; i++) {
        ASSERT(bytes[i] == (unsigned char)(i & 0xFF));
    }
    
    memento_free(alloc, result.ptr);
    memento_destroy_allocator(alloc);
}

int main(void) {
    printf("Memento Leak Detection Tests\n");
    printf("============================\n");
    
    if (memento_init() != MEMENTO_SUCCESS) {
        printf("Failed to initialize memento\n");
        return 1;
    }
    
    RUN_TEST(basic_leak_detection);
    RUN_TEST(proxy_leak_tracking);
    RUN_TEST(hierarchical_leak_detection);
    RUN_TEST(statistics_integrity);
    RUN_TEST(failed_allocation_tracking);
    RUN_TEST(memory_corruption_detection);
    
    memento_shutdown();
    
    printf("\n========================================\n");
    printf("Test Summary:\n");
    printf("  Total: %d\n", test_count);
    printf("  Passed: %d\n", pass_count);
    printf("  Failed: %d\n", test_count - pass_count);
    printf("========================================\n");
    
    return (test_count == pass_count) ? 0 : 1;
}

