/*
 * Memory throughput benchmark for memento allocator
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#define MEMENTO_IMPLEMENTATION
#include "../memento.h"

#define THROUGHPUT_SIZE (100 * 1024 * 1024)  /* 100MB */
#define CHUNK_SIZE 4096

typedef struct {
    const char* name;
    void* (*alloc_func)(size_t size);
    void (*free_func)(void* ptr);
    void* allocator;
} allocator_info_t;

static void* malloc_wrapper(size_t size) {
    return malloc(size);
}

static void* memento_thread_cache_alloc(size_t size) {
    static memento_allocator_t* alloc = NULL;
    if (!alloc) {
        alloc = memento_create_thread_cache("throughput_thread");
    }
    memento_result_t result = memento_alloc(alloc, size);
    return result.success ? result.ptr : NULL;
}

static void* memento_block_alloc(size_t size) {
    static memento_allocator_t* alloc = NULL;
    static memento_allocator_t* backing = NULL;
    if (!alloc) {
        backing = memento_create_thread_cache("throughput_block_back");
        alloc = memento_create_block_allocator("throughput_block", backing);
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
        alloc = memento_create_thread_cache("throughput_thread");
    }
    memento_free(alloc, ptr);
}

static void memento_block_free(void* ptr) {
    static memento_allocator_t* alloc = NULL;
    if (!alloc) {
        alloc = memento_create_thread_cache("throughput_thread");
        memento_allocator_t* backing = memento_create_thread_cache("throughput_block_back");
        alloc = memento_create_block_allocator("throughput_block", backing);
    }
    memento_free(alloc, ptr);
}

static double benchmark_throughput(allocator_info_t* allocator, size_t total_size, size_t chunk_size) {
    int num_chunks = total_size / chunk_size;
    void** chunks = malloc(num_chunks * sizeof(void*));
    if (!chunks) {
        printf("Failed to allocate chunk array\n");
        return -1.0;
    }
    
    clock_t start = clock();
    
    /* Allocate all chunks */
    for (int i = 0; i < num_chunks; i++) {
        chunks[i] = allocator->alloc_func(chunk_size);
        if (!chunks[i]) {
            printf("Allocation failed at chunk %d\n", i);
            free(chunks);
            return -1.0;
        }
        
        /* Write data to ensure memory is actually allocated */
        memset(chunks[i], i & 0xFF, chunk_size);
    }
    
    /* Read data to ensure memory is accessible */
    for (int i = 0; i < num_chunks; i++) {
        volatile unsigned char val = ((unsigned char*)chunks[i])[0];
        (void)val; /* Suppress unused variable warning */
    }
    
    /* Free all chunks */
    for (int i = 0; i < num_chunks; i++) {
        allocator->free_func(chunks[i]);
    }
    
    clock_t end = clock();
    free(chunks);
    
    double cpu_time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
    return cpu_time_used;
}

static void run_throughput_benchmark(size_t chunk_size, const char* chunk_desc) {
    printf("\n=== Chunk Size: %s ===\n", chunk_desc);
    
    allocator_info_t allocators[] = {
        {"malloc", malloc_wrapper, malloc_free_wrapper, NULL},
        {"memento_thread_cache", memento_thread_cache_alloc, memento_thread_cache_free, NULL},
        {"memento_block", memento_block_alloc, memento_block_free, NULL},
    };
    
    printf("Total Size: %.1f MB, Chunk Size: %s\n", 
           THROUGHPUT_SIZE / (1024.0 * 1024.0), chunk_desc);
    printf("%-25s %-15s %-15s %-15s\n", "Allocator", "Time (s)", "Throughput (MB/s)", "Chunks/sec");
    printf("%-25s %-15s %-15s %-15s\n", 
           "-------------------------", "---------------", "---------------", "---------------");
    
    for (size_t i = 0; i < sizeof(allocators)/sizeof(allocators[0]); i++) {
        double time = benchmark_throughput(&allocators[i], THROUGHPUT_SIZE, chunk_size);
        if (time > 0) {
            double throughput_mbps = (THROUGHPUT_SIZE / (1024.0 * 1024.0)) / time;
            double chunks_per_sec = (THROUGHPUT_SIZE / chunk_size) / time;
            
            printf("%-25s %-15.3f %-15.1f %-15.0f\n", 
                   allocators[i].name, time, throughput_mbps, chunks_per_sec);
        }
    }
}

static void run_pattern_benchmark(void) {
    printf("\n=== Memory Access Pattern Benchmark ===\n");
    
    static memento_allocator_t* alloc = NULL;
    if (!alloc) {
        alloc = memento_create_thread_cache("pattern_bench");
    }
    
    size_t size = 10 * 1024 * 1024;  /* 10MB */
    int iterations = 100;
    
    printf("Size: %.1f MB, Iterations: %d\n", size / (1024.0 * 1024.0), iterations);
    printf("%-25s %-15s %-15s\n", "Pattern", "Time (s)", "Throughput (MB/s)");
    printf("%-25s %-15s %-15s\n", "-------------------------", "---------------", "---------------");
    
    /* Sequential write */
    {
        memento_result_t result = memento_alloc(alloc, size);
        if (result.success) {
            clock_t start = clock();
            
            for (int iter = 0; iter < iterations; iter++) {
                memset(result.ptr, iter & 0xFF, size);
            }
            
            clock_t end = clock();
            double time = ((double)(end - start)) / CLOCKS_PER_SEC;
            double throughput = (size * iterations / (1024.0 * 1024.0)) / time;
            
            printf("%-25s %-15.3f %-15.1f\n", "Sequential Write", time, throughput);
            memento_free(alloc, result.ptr);
        }
    }
    
    /* Random access */
    {
        memento_result_t result = memento_alloc(alloc, size);
        if (result.success) {
            srand(42); /* Fixed seed for reproducibility */
            int num_accesses = size / 64;  /* Access every 64 bytes */
            
            clock_t start = clock();
            
            for (int iter = 0; iter < iterations; iter++) {
                for (int i = 0; i < num_accesses; i++) {
                    int offset = (rand() % num_accesses) * 64;
                    ((unsigned char*)result.ptr)[offset] = (unsigned char)(iter & 0xFF);
                }
            }
            
            clock_t end = clock();
            double time = ((double)(end - start)) / CLOCKS_PER_SEC;
            double throughput = (size * iterations / (1024.0 * 1024.0)) / time;
            
            printf("%-25s %-15.3f %-15.1f\n", "Random Access", time, throughput);
            memento_free(alloc, result.ptr);
        }
    }
}

int main(void) {
    printf("Memento Memory Throughput Benchmark\n");
    printf("====================================\n");
    printf("Total Size: %.1f MB\n\n", THROUGHPUT_SIZE / (1024.0 * 1024.0));
    
    if (memento_init() != MEMENTO_SUCCESS) {
        printf("Failed to initialize memento\n");
        return 1;
    }
    
    /* Run throughput benchmarks for different chunk sizes */
    run_throughput_benchmark(64, "64 bytes");
    run_throughput_benchmark(256, "256 bytes");
    run_throughput_benchmark(1024, "1KB");
    run_throughput_benchmark(4096, "4KB");
    run_throughput_benchmark(16384, "16KB");
    
    /* Memory access pattern benchmark */
    run_pattern_benchmark();
    
    memento_shutdown();
    
    return 0;
}