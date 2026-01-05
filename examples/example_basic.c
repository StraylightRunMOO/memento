/*
 * Basic usage example for memento allocator
 * 
 * This example demonstrates the simplest possible usage of the memento
 * allocator library, including initialization, allocation, and cleanup.
 */

#include <stdio.h>
#include <string.h>

#define MEMENTO_IMPLEMENTATION
#include "../memento.h"

int main(void) {
    printf("Memento Basic Usage Example\n");
    printf("===========================\n\n");
    
    /* Step 1: Initialize the memento library */
    printf("1. Initializing memento library...\n");
    if (memento_init() != MEMENTO_SUCCESS) {
        printf("   Failed to initialize memento!\n");
        return 1;
    }
    printf("   Memento initialized successfully\n\n");
    
    /* Step 2: Create a thread cache allocator */
    printf("2. Creating thread cache allocator...\n");
    memento_allocator_t* allocator = memento_create_thread_cache("basic_example");
    if (!allocator) {
        printf("   Failed to create allocator!\n");
        memento_shutdown();
        return 1;
    }
    printf("   Created allocator: %s\n\n", allocator->name);
    
    /* Step 3: Allocate some memory */
    printf("3. Allocating memory...\n");
    size_t size = 1024;  /* 1KB */
    memento_result_t result = memento_alloc(allocator, size);
    
    if (!result.success || !result.ptr) {
        printf("   Failed to allocate %zu bytes!\n", size);
        memento_destroy_allocator(allocator);
        memento_shutdown();
        return 1;
    }
    
    printf("   Allocated %zu bytes at address: %p\n", result.size, result.ptr);
    
    /* Step 4: Use the allocated memory */
    printf("4. Using allocated memory...\n");
    strcpy((char*)result.ptr, "Hello, Memento!");
    printf("   Written data: %s\n", (char*)result.ptr);
    
    /* Step 5: Check statistics */
    printf("\n5. Allocator statistics:\n");
    const memento_stats_t* stats = memento_get_stats(allocator);
    if (stats) {
        printf("   Allocations: %zu\n", stats->allocation_count);
        printf("   Current usage: %zu bytes\n", stats->current_usage);
        printf("   Peak usage: %zu bytes\n", stats->peak_usage);
    }
    
    /* Step 6: Free the memory */
    printf("\n6. Freeing memory...\n");
    memento_free(allocator, result.ptr);
    printf("   Memory freed successfully\n");
    
    /* Step 7: Clean up */
    printf("\n7. Cleaning up...\n");
    memento_destroy_allocator(allocator);
    printf("   Allocator destroyed\n");
    
    memento_shutdown();
    printf("   Memento shut down\n");
    
    printf("\n✓ Basic example completed successfully!\n");
    return 0;
}