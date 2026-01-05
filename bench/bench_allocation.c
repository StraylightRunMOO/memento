/*
 * Allocation speed benchmark for memento allocator
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#define MEMENTO_IMPLEMENTATION
#include "../memento.h"

typedef struct {
    const char* name;
    void* (*alloc_func)(size_t size);
    void (*free_func)(void* ptr);
} allocator_info_t;

static void* malloc_wrapper(size_t size) {
    return malloc(size);
}

static void* memento_thread_cache_alloc(size_t size) {
    static memento_allocator_t* alloc = NULL;
    if (!alloc) {
        alloc = memento_create_thread_cache("bench_thread");
    }
    memento_result_t result = memento_alloc(alloc, size);
    return result.success ? result.ptr : NULL;
}

static void* memento_block_alloc(size_t size) {
    static memento_allocator_t* alloc = NULL;
    static memento_allocator_t* backing = NULL;
    if (!alloc) {
        backing = memento_create_thread_cache("bench_block_back");
        alloc = memento_create_block_allocator("bench_block", backing);
    }
    memento_result_t result = memento_alloc(alloc, size);
    return result.success ? result.ptr : NULL;
}

static void malloc_free_wrapper(void* ptr) {
    free(ptr);
}

static void memento_thread_cache_free(void* ptr) {
    static memento_allocator_t* alloc = NULL;
    if (!alloc) {
        alloc = memento_create_thread_cache("bench_thread");
    }
    memento_free(alloc, ptr);
}

static void memento_block_free(void* ptr) {
    static memento_allocator_t* alloc = NULL;
    if (!alloc) {
        alloc = memento_create_thread_cache("bench_thread");
        memento_allocator_t* backing = memento_create_thread_cache("bench_block_back");
        alloc = memento_create_block_allocator("bench_block", backing);
    }
    memento_free(alloc, ptr);
}

static double benchmark_allocation(allocator_info_t* allocator, size_t size, int iterations) {
    clock_t start = clock();
    
    for (int i = 0; i < iterations; i++) {
        void* ptr = allocator->alloc_func(size);
        if (!ptr) {
            printf("Allocation failed\n");
            return -1.0;
        }
        allocator->free_func(ptr);
    }
    
    clock_t end = clock();
    double cpu_time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
    return cpu_time_used;
}

static void run_allocation_benchmark(size_t size, const char* size_desc) {
    printf("\n=== Allocation Size: %s ===\n", size_desc);
    
    allocator_info_t allocators[] = {
        {"malloc", malloc_wrapper, malloc_free_wrapper},
        {"memento_thread_cache", memento_thread_cache_alloc, memento_thread_cache_free},
        {"memento_block", memento_block_alloc, memento_block_free},
    };
    
    int iterations = 100000;
    
    printf("Iterations: %d\n", iterations);
    printf("%-25s %-15s %-15s %-15s\n", "Allocator", "Total Time (s)", "Time/Alloc (μs)", "Allocs/sec");
    printf("%-25s %-15s %-15s %-15s\n", 
           "-------------------------", "---------------", "---------------", "---------------");
    
    for (size_t i = 0; i < sizeof(allocators)/sizeof(allocators[0]); i++) {
        double time = benchmark_allocation(&allocators[i], size, iterations);
        if (time > 0) {
            double time_per_alloc = (time * 1000000.0) / iterations;
            double allocs_per_sec = iterations / time;
            
            printf("%-25s %-15.6f %-15.3f %-15.0f\n", 
                   allocators[i].name, time, time_per_alloc, allocs_per_sec);
        }
    }
}

static void run_aligned_allocation_benchmark(void) {
    printf("\n=== Aligned Allocation Benchmark ===\n");
    
    static memento_allocator_t* alloc = NULL;
    if (!alloc) {
        alloc = memento_create_thread_cache("aligned_bench");
    }
    
    size_t alignments[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    int iterations = 50000;
    
    printf("Iterations: %d, Size: 1024 bytes\n", iterations);
    printf("%-15s %-15s %-15s\n", "Alignment", "Total Time (s)", "Time/Alloc (μs)");
    printf("%-15s %-15s %-15s\n", "---------------", "---------------", "---------------");
    
    for (size_t i = 0; i < sizeof(alignments)/sizeof(alignments[0]); i++) {
        clock_t start = clock();
        
        for (int j = 0; j < iterations; j++) {
            memento_result_t result = memento_alloc_aligned(alloc, 1024, alignments[i]);
            if (!result.success || !result.ptr) {
                printf("Aligned allocation failed\n");
                return;
            }
            memento_free(alloc, result.ptr);
        }
        
        clock_t end = clock();
        double time = ((double)(end - start)) / CLOCKS_PER_SEC;
        double time_per_alloc = (time * 1000000.0) / iterations;
        
        printf("%-15zu %-15.6f %-15.3f\n", alignments[i], time, time_per_alloc);
    }
}

int main(void) {
    printf("Memento Allocation Speed Benchmark\n");
    printf("====================================\n");
    
    if (memento_init() != MEMENTO_SUCCESS) {
        printf("Failed to initialize memento\n");
        return 1;
    }
    
    /* Run benchmarks for different allocation sizes */
    run_allocation_benchmark(64, "64 bytes (small)");
    run_allocation_benchmark(256, "256 bytes (medium)");
    run_allocation_benchmark(1024, "1KB (large)");
    run_allocation_benchmark(4096, "4KB (very large)");
    run_allocation_benchmark(16384, "16KB (huge)");
    
    /* Aligned allocation benchmark */
    run_aligned_allocation_benchmark();
    
    memento_shutdown();
    
    return 0;
}