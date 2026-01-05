/*
 * Fragmentation benchmark for memento allocator
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#define MEMENTO_IMPLEMENTATION
#include "../memento.h"

#define FRAGMENTATION_ITERATIONS 1000
#define MAX_FRAGMENT_SIZE 8192

static void benchmark_fragmentation_pattern(const char* name, memento_allocator_t* allocator, 
                                            void (*pattern_func)(memento_allocator_t*)) {
    printf("\n=== %s Fragmentation Pattern ===\n", name);
    
    clock_t start = clock();
    pattern_func(allocator);
    clock_t end = clock();
    
    double time = ((double)(end - start)) / CLOCKS_PER_SEC;
    printf("Total time: %.3f seconds\n", time);
    
    /* Show final statistics */
    const memento_stats_t* stats = memento_get_stats(allocator);
    if (stats) {
        printf("Final statistics:\n");
        printf("  Allocations: %zu\n", stats->allocation_count);
        printf("  Current usage: %zu bytes\n", stats->current_usage);
        printf("  Peak usage: %zu bytes\n", stats->peak_usage);
    }
}

static void alternating_size_pattern(memento_allocator_t* allocator) {
    printf("Creating alternating size pattern...\n");
    
    void* ptrs[100];
    
    /* Allocate alternating small and large blocks */
    for (int i = 0; i < 100; i++) {
        size_t size = (i % 2 == 0) ? 64 : 1024;
        memento_result_t result = memento_alloc(allocator, size);
        if (result.success) {
            ptrs[i] = result.ptr;
            memset(result.ptr, i & 0xFF, size);
        } else {
            ptrs[i] = NULL;
        }
    }
    
    /* Free every other allocation to create fragmentation */
    printf("Freeing every other allocation...\n");
    for (int i = 0; i < 100; i += 2) {
        if (ptrs[i]) {
            memento_free(allocator, ptrs[i]);
        }
    }
    
    /* Try to allocate in fragmented space */
    printf("Testing fragmented allocation...\n");
    int successful = 0;
    for (int i = 0; i < 50; i++) {
        size_t size = 64;  /* Same size as freed blocks */
        memento_result_t result = memento_alloc(allocator, size);
        if (result.success) {
            successful++;
            memset(result.ptr, 0xFF, size);
            memento_free(allocator, result.ptr);
        }
    }
    printf("Successfully allocated %d blocks in fragmented space\n", successful);
    
    /* Clean up remaining */
    for (int i = 1; i < 100; i += 2) {
        if (ptrs[i]) {
            memento_free(allocator, ptrs[i]);
        }
    }
}

static void random_pattern(memento_allocator_t* allocator) {
    printf("Creating random allocation pattern...\n");
    srand(42);  /* Fixed seed for reproducibility */
    
    void* ptrs[200];
    size_t sizes[200];
    
    /* Random allocations */
    for (int i = 0; i < 200; i++) {
        sizes[i] = (rand() % MAX_FRAGMENT_SIZE) + 64;
        memento_result_t result = memento_alloc(allocator, sizes[i]);
        if (result.success) {
            ptrs[i] = result.ptr;
            memset(result.ptr, i & 0xFF, sizes[i]);
        } else {
            ptrs[i] = NULL;
        }
    }
    
    /* Random deallocations */
    printf("Performing random deallocations...\n");
    for (int i = 0; i < 100; i++) {
        int index = rand() % 200;
        if (ptrs[index]) {
            memento_free(allocator, ptrs[index]);
            ptrs[index] = NULL;
        }
    }
    
    /* Try mixed allocations */
    printf("Testing mixed allocation sizes...\n");
    for (int i = 0; i < 50; i++) {
        size_t size = (rand() % 1024) + 64;
        memento_result_t result = memento_alloc(allocator, size);
        if (result.success) {
            memset(result.ptr, 0xFF, size);
            memento_free(allocator, result.ptr);
        }
    }
    
    /* Clean up remaining */
    for (int i = 0; i < 200; i++) {
        if (ptrs[i]) {
            memento_free(allocator, ptrs[i]);
        }
    }
}

static void lifecycle_pattern(memento_allocator_t* allocator) {
    printf("Simulating object lifecycle pattern...\n");
    
    /* Simulate objects being created and destroyed over time */
    for (int cycle = 0; cycle < 10; cycle++) {
        printf("  Cycle %d:\n", cycle + 1);
        
        /* Create some objects */
        void* objects[50];
        for (int i = 0; i < 50; i++) {
            size_t size = 256 + (i * 16);  /* Gradually increasing sizes */
            memento_result_t result = memento_alloc(allocator, size);
            if (result.success) {
                objects[i] = result.ptr;
                memset(result.ptr, cycle & 0xFF, size);
            } else {
                objects[i] = NULL;
            }
        }
        
        /* Some objects live longer than others */
        int survivors = 30 - (cycle * 2);  /* Fewer survivors each cycle */
        if (survivors < 5) survivors = 5;
        
        /* Destroy some objects */
        for (int i = survivors; i < 50; i++) {
            if (objects[i]) {
                memento_free(allocator, objects[i]);
                objects[i] = NULL;
            }
        }
        
        printf("    Created 50 objects, kept %d alive\n", survivors);
        
        /* Keep survivors for next cycle */
        for (int i = 0; i < survivors; i++) {
            if (objects[i]) {
                /* Objects survive to next cycle */
            }
        }
    }
}

int main(void) {
    printf("Memento Fragmentation Benchmark\n");
    printf("===============================\n");
    
    if (memento_init() != MEMENTO_SUCCESS) {
        printf("Failed to initialize memento\n");
        return 1;
    }
    
    /* Test with thread cache allocator */
    memento_allocator_t* thread_cache = memento_create_thread_cache("frag_thread");
    benchmark_fragmentation_pattern("Thread Cache", thread_cache, alternating_size_pattern);
    benchmark_fragmentation_pattern("Thread Cache", thread_cache, random_pattern);
    benchmark_fragmentation_pattern("Thread Cache", thread_cache, lifecycle_pattern);
    memento_destroy_allocator(thread_cache);
    
    /* Test with block allocator */
    memento_allocator_t* backing = memento_create_thread_cache("frag_back");
    memento_allocator_t* block = memento_create_block_allocator("frag_block", backing);
    benchmark_fragmentation_pattern("Block", block, alternating_size_pattern);
    benchmark_fragmentation_pattern("Block", block, random_pattern);
    benchmark_fragmentation_pattern("Block", block, lifecycle_pattern);
    memento_destroy_allocator(block);
    memento_destroy_allocator(backing);
    
    memento_shutdown();
    
    return 0;
}