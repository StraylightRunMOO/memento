/*
 * Statistics and debugging example
 * 
 * This example demonstrates how to use memento's statistics and debugging
 * features to track memory usage, detect leaks, and analyze allocation patterns.
 */

#include <stdio.h>
#include <string.h>

#define MEMENTO_IMPLEMENTATION
#include "../include/memento.h"

static void demonstrate_basic_statistics(void) {
    printf("  Basic Statistics:\n");
    
    memento_allocator_t* alloc = memento_create_thread_cache("stats_basic");
    if (!alloc) {
        printf("    Failed to create allocator!\n");
        return;
    }
    
    /* Get initial statistics */
    const memento_stats_t* stats = memento_get_stats(alloc);
    printf("    Initial state:\n");
    printf("      Allocations: %zu\n", stats->allocation_count);
    printf("      Current usage: %zu bytes\n", stats->current_usage);
    printf("      Peak usage: %zu bytes\n", stats->peak_usage);
    printf("      Failed allocations: %zu\n", stats->failed_allocations);
    
    /* Perform some allocations */
    void* ptr1 = memento_alloc(alloc, 1024).ptr;
    void* ptr2 = memento_alloc(alloc, 2048).ptr;
    void* ptr3 = memento_alloc(alloc, 4096).ptr;
    
    stats = memento_get_stats(alloc);
    printf("    \n    After 3 allocations:\n");
    printf("      Allocations: %zu\n", stats->allocation_count);
    printf("      Current usage: %zu bytes\n", stats->current_usage);
    printf("      Peak usage: %zu bytes\n", stats->peak_usage);
    
    /* Free some memory */
    memento_free(alloc, ptr1);
    memento_free(alloc, ptr3);
    
    stats = memento_get_stats(alloc);
    printf("    \n    After freeing 2 allocations:\n");
    printf("      Allocations: %zu\n", stats->allocation_count);
    printf("      Deallocations: %zu\n", stats->deallocation_count);
    printf("      Current usage: %zu bytes\n", stats->current_usage);
    printf("      Total allocated: %zu bytes\n", stats->total_allocated);
    printf("      Total deallocated: %zu bytes\n", stats->total_deallocated);
    
    /* Free remaining allocation */
    memento_free(alloc, ptr2);
    
    memento_destroy_allocator(alloc);
    printf("    Basic statistics demo completed\n");
}

static void demonstrate_proxy_statistics(void) {
    printf("\n  Proxy Statistics:\n");
    
    memento_allocator_t* backing = memento_create_thread_cache("stats_proxy_back");
    memento_allocator_t* proxy = memento_create_proxy_allocator("stats_proxy", backing);
    
    if (!backing || !proxy) {
        printf("    Failed to create allocators!\n");
        if (backing) memento_destroy_allocator(backing);
        return;
    }
    
    printf("    Both proxy and backing track statistics independently\n");
    
    /* Allocate through proxy */
    void* ptr = memento_alloc(proxy, 2048).ptr;
    if (ptr) {
        const memento_stats_t* proxy_stats = memento_get_stats(proxy);
        const memento_stats_t* backing_stats = memento_get_stats(backing);
        
        printf("    After allocation through proxy:\n");
        printf("      Proxy allocations: %zu\n", proxy_stats->allocation_count);
        printf("      Backing allocations: %zu\n", backing_stats->allocation_count);
        printf("      Proxy current usage: %zu bytes\n", proxy_stats->current_usage);
        printf("      Backing current usage: %zu bytes\n", backing_stats->current_usage);
        
        memento_free(proxy, ptr);
        
        proxy_stats = memento_get_stats(proxy);
        backing_stats = memento_get_stats(backing);
        printf("    \n    After deallocation:\n");
        printf("      Proxy deallocations: %zu\n", proxy_stats->deallocation_count);
        printf("      Backing deallocations: %zu\n", backing_stats->deallocation_count);
    }
    
    memento_destroy_allocator(proxy);
    memento_destroy_allocator(backing);
    printf("    Proxy statistics demo completed\n");
}

static void demonstrate_allocation_patterns(void) {
    printf("\n  Allocation Patterns:\n");
    
    memento_allocator_t* alloc = memento_create_thread_cache("stats_patterns");
    if (!alloc) {
        printf("    Failed to create allocator!\n");
        return;
    }
    
    printf("    Simulating different allocation patterns...\n");
    
    /* Pattern 1: Many small allocations */
    printf("    Pattern 1: Many small allocations (64 bytes each)\n");
    for (int i = 0; i < 10; i++) {
        void* ptr = memento_alloc(alloc, 64).ptr;
        if (ptr) {
            /* Use the memory */
            memset(ptr, i, 64);
            memento_free(alloc, ptr);
        }
    }
    
    const memento_stats_t* stats = memento_get_stats(alloc);
    printf("      After small allocations: %zu allocations\n", stats->allocation_count);
    
    /* Pattern 2: Few large allocations */
    printf("    Pattern 2: Few large allocations (4KB each)\n");
    void* large_ptrs[3];
    for (int i = 0; i < 3; i++) {
        large_ptrs[i] = memento_alloc(alloc, 4 * 1024).ptr;
        if (large_ptrs[i]) {
            memset(large_ptrs[i], i, 4 * 1024);
        }
    }
    
    stats = memento_get_stats(alloc);
    printf("      After large allocations: %zu allocations, %zu bytes current usage\n",
           stats->allocation_count, stats->current_usage);
    printf("      Peak usage: %zu bytes\n", stats->peak_usage);
    
    /* Free large allocations */
    for (int i = 0; i < 3; i++) {
        if (large_ptrs[i]) {
            memento_free(alloc, large_ptrs[i]);
        }
    }
    
    /* Pattern 3: Mixed sizes */
    printf("    Pattern 3: Mixed allocation sizes\n");
    size_t sizes[] = {128, 256, 512, 1024, 2048};
    for (int i = 0; i < 5; i++) {
        void* ptr = memento_alloc(alloc, sizes[i]).ptr;
        if (ptr) {
            memset(ptr, i, sizes[i]);
            memento_free(alloc, ptr);
        }
    }
    
    stats = memento_get_stats(alloc);
    printf("      Final statistics:\n");
    printf("        Total allocations: %zu\n", stats->total_allocated);
    printf("        Total deallocations: %zu\n", stats->total_deallocated);
    printf("        Memory leaks: %zu bytes\n", 
           stats->total_allocated - stats->total_deallocated);
    printf("        Failed allocations: %zu\n", stats->failed_allocations);
    
    memento_destroy_allocator(alloc);
    printf("    Allocation patterns demo completed\n");
}

static void demonstrate_failed_allocations(void) {
    printf("\n  Failed Allocations:\n");
    
    memento_allocator_t* alloc = memento_create_thread_cache("stats_failures");
    if (!alloc) {
        printf("    Failed to create allocator!\n");
        return;
    }
    
    const memento_stats_t* stats = memento_get_stats(alloc);
    size_t initial_failed = stats->failed_allocations;
    
    printf("    Testing failed allocation scenarios...\n");
    
    /* Test 1: Invalid alignment */
    printf("    Test 1: Invalid alignment (3 bytes, not power of 2)\n");
    memento_result_t result = memento_alloc_aligned(alloc, 1024, 3);
    if (!result.success) {
        printf("      Allocation failed as expected\n");
    }
    
    /* Test 2: Excessive alignment */
    printf("    Test 2: Excessive alignment\n");
    result = memento_alloc_aligned(alloc, 1024, MEMENTO_MAX_ALIGNMENT * 2);
    if (!result.success) {
        printf("      Allocation failed as expected\n");
    }
    
    /* Test 3: Null allocator */
    printf("    Test 3: Null allocator\n");
    result = memento_alloc(NULL, 1024);
    if (!result.success) {
        printf("      Allocation failed as expected\n");
    }
    
    stats = memento_get_stats(alloc);
    printf("    \n    Failed allocations: %zu (initial: %zu)\n", 
           stats->failed_allocations, initial_failed);
    printf("    Note: Invalid parameters don't count as failed allocations\n");
    printf("    They're rejected before reaching the allocator\n");
    
    memento_destroy_allocator(alloc);
    printf("    Failed allocations demo completed\n");
}

static void demonstrate_print_statistics(void) {
    printf("\n  Print Statistics:\n");
    
    memento_allocator_t* alloc = memento_create_thread_cache("stats_print");
    if (!alloc) {
        printf("    Failed to create allocator!\n");
        return;
    }
    
    printf("    Creating some allocations...\n");
    
    /* Create various allocations */
    void* ptrs[5];
    size_t sizes[] = {128, 256, 512, 1024, 2048};
    
    for (int i = 0; i < 5; i++) {
        memento_result_t result = memento_alloc(alloc, sizes[i]);
        if (result.success) {
            ptrs[i] = result.ptr;
            memset(ptrs[i], i, sizes[i]);
        } else {
            ptrs[i] = NULL;
        }
    }
    
    /* Show detailed statistics */
    printf("    Detailed statistics:\n");
    memento_print_stats(alloc);
    
    /* Clean up */
    for (int i = 0; i < 5; i++) {
        if (ptrs[i]) {
            memento_free(alloc, ptrs[i]);
        }
    }
    
    printf("    Final statistics:\n");
    memento_print_stats(alloc);
    
    memento_destroy_allocator(alloc);
    printf("    Print statistics demo completed\n");
}

int main(void) {
    printf("Memento Statistics and Debugging Example\n");
    printf("=======================================\n\n");
    
    /* Initialize memento */
    if (memento_init() != MEMENTO_SUCCESS) {
        printf("Failed to initialize memento!\n");
        return 1;
    }
    
    printf("This example demonstrates memento's statistics and debugging features:\n");
    printf("- Basic allocation statistics\n");
    printf("- Proxy allocator statistics\n");
    printf("- Allocation pattern analysis\n");
    printf("- Failed allocation tracking\n");
    printf("- Detailed statistics printing\n\n");
    
    /* Run all demonstrations */
    demonstrate_basic_statistics();
    demonstrate_proxy_statistics();
    demonstrate_allocation_patterns();
    demonstrate_failed_allocations();
    demonstrate_print_statistics();
    
    printf("\n✓ Statistics and debugging example completed successfully!\n");
    printf("\nKey takeaways:\n");
    printf("- Memento provides comprehensive statistics tracking\n");
    printf("- Proxy allocators add hierarchical memory visibility\n");
    printf("- Failed allocations are tracked for debugging\n");
    printf("- Statistics help identify memory usage patterns and leaks\n");
    
    memento_shutdown();
    return 0;
}
