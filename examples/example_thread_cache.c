/*
 * Thread cache allocator example
 * 
 * This example demonstrates the thread cache allocator, which is optimized
 * for high-performance, general-purpose allocation with thread-local caching.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEMENTO_IMPLEMENTATION
#include "../memento.h"

/* Simulate some work that allocates memory */
static void do_work(memento_allocator_t* allocator, int worker_id) {
    printf("  Worker %d: Starting work...\n", worker_id);
    
    /* Allocate some objects */
    struct DataObject {
        int id;
        char name[64];
        double value;
    };
    
    /* Allocate a single object */
    memento_result_t obj_result = memento_alloc(allocator, sizeof(struct DataObject));
    if (obj_result.success) {
        struct DataObject* obj = (struct DataObject*)obj_result.ptr;
        obj->id = worker_id;
        snprintf(obj->name, sizeof(obj->name), "Worker-%d-Object", worker_id);
        obj->value = worker_id * 3.14159;
        
        printf("    Worker %d: Created object: id=%d, name=%s, value=%.2f\n", 
               worker_id, obj->id, obj->name, obj->value);
        
        /* Allocate an array */
        int array_size = 10 + worker_id;
        memento_result_t array_result = memento_alloc(allocator, array_size * sizeof(int));
        if (array_result.success) {
            int* array = (int*)array_result.ptr;
            
            for (int i = 0; i < array_size; i++) {
                array[i] = worker_id * 100 + i;
            }
            
            printf("    Worker %d: Created array with %d elements\n", worker_id, array_size);
            
            /* Free the array */
            memento_free(allocator, array);
        }
        
        /* Free the object */
        memento_free(allocator, obj);
    }
    
    printf("  Worker %d: Work completed\n", worker_id);
}

int main(void) {
    printf("Memento Thread Cache Allocator Example\n");
    printf("======================================\n\n");
    
    /* Initialize memento */
    if (memento_init() != MEMENTO_SUCCESS) {
        printf("Failed to initialize memento!\n");
        return 1;
    }
    
    printf("1. Creating thread cache allocator...\n");
    memento_allocator_t* allocator = memento_create_thread_cache("thread_cache_example");
    if (!allocator) {
        printf("Failed to create allocator!\n");
        memento_shutdown();
        return 1;
    }
    printf("   Created allocator: %s\n\n", allocator->name);
    
    /* Demonstrate different allocation patterns */
    printf("2. Demonstrating allocation patterns...\n");
    
    /* Small allocations */
    printf("   Small allocations (64 bytes each):\n");
    for (int i = 0; i < 5; i++) {
        memento_result_t result = memento_alloc(allocator, 64);
        if (result.success) {
            memset(result.ptr, i, 64);
            printf("     Allocated %zu bytes at %p\n", result.size, result.ptr);
            memento_free(allocator, result.ptr);
        }
    }
    
    /* Medium allocations */
    printf("\n   Medium allocations (1KB each):\n");
    for (int i = 0; i < 3; i++) {
        memento_result_t result = memento_alloc(allocator, 1024);
        if (result.success) {
            memset(result.ptr, i, 1024);
            printf("     Allocated %zu bytes at %p\n", result.size, result.ptr);
            memento_free(allocator, result.ptr);
        }
    }
    
    /* Large allocations */
    printf("\n   Large allocations (64KB each):\n");
    for (int i = 0; i < 2; i++) {
        memento_result_t result = memento_alloc(allocator, 64 * 1024);
        if (result.success) {
            memset(result.ptr, i, 64 * 1024);
            printf("     Allocated %zu bytes at %p\n", result.size, result.ptr);
            memento_free(allocator, result.ptr);
        }
    }
    
    /* Simulate work with structured data */
    printf("\n3. Simulating work with structured data...\n");
    for (int i = 1; i <= 3; i++) {
        do_work(allocator, i);
    }
    
    /* Show final statistics */
    printf("\n4. Final allocator statistics:\n");
    const memento_stats_t* stats = memento_get_stats(allocator);
    if (stats) {
        printf("   Total allocations: %zu\n", stats->allocation_count);
        printf("   Total deallocations: %zu\n", stats->deallocation_count);
        printf("   Current usage: %zu bytes\n", stats->current_usage);
        printf("   Peak usage: %zu bytes\n", stats->peak_usage);
        printf("   Failed allocations: %zu\n", stats->failed_allocations);
        printf("   Thread cache hits: %zu\n", stats->thread_cache_hits);
        printf("   Thread cache misses: %zu\n", stats->thread_cache_misses);
    }
    
    /* Clean up */
    printf("\n5. Cleaning up...\n");
    memento_destroy_allocator(allocator);
    memento_shutdown();
    
    printf("\n✓ Thread cache example completed successfully!\n");
    return 0;
}