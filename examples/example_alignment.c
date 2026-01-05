/*
 * Memory alignment example
 * 
 * This example demonstrates how to use memento's alignment features
 * for different data types and alignment requirements.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define MEMENTO_IMPLEMENTATION
#include "../include/memento.h"

/* Data structures with different alignment requirements */
typedef struct {
    char data[16];  /* 16-byte aligned for SIMD operations */
} AlignedData16;

typedef struct {
    float x, y, z, w;  /* 16-byte aligned for SSE operations */
} Vector4;

typedef struct {
    double values[2];  /* 16-byte aligned for AVX operations */
} DoubleVector;

typedef struct {
    uint64_t data[4];  /* 32-byte aligned for cache line optimization */
} CacheLineData;

static void demonstrate_basic_alignment(void) {
    printf("  Basic Alignment:\n");
    
    memento_allocator_t* alloc = memento_create_thread_cache("align_basic");
    if (!alloc) {
        printf("    Failed to create allocator!\n");
        return;
    }
    
    /* Test different alignment values */
    size_t alignments[] = {1, 2, 4, 8, 16, 32, 64, 128};
    
    for (size_t i = 0; i < sizeof(alignments)/sizeof(alignments[0]); i++) {
        size_t alignment = alignments[i];
        memento_result_t result = memento_alloc_aligned(alloc, 256, alignment);
        
        if (result.success) {
            uintptr_t ptr_val = (uintptr_t)result.ptr;
            int is_aligned = (ptr_val % alignment) == 0;
            
            printf("    Alignment %3zu: %s at %p (aligned: %s)\n", 
                   alignment, result.success ? "SUCCESS" : "FAILED", 
                   result.ptr, is_aligned ? "YES" : "NO");
            
            if (result.ptr) {
                memento_free(alloc, result.ptr);
            }
        } else {
            printf("    Alignment %3zu: FAILED\n", alignment);
        }
    }
    
    memento_destroy_allocator(alloc);
    printf("    Basic alignment demo completed\n");
}

static void demonstrate_struct_alignment(void) {
    printf("\n  Structure Alignment:\n");
    
    memento_allocator_t* alloc = memento_create_thread_cache("align_struct");
    if (!alloc) {
        printf("    Failed to create allocator!\n");
        return;
    }
    
    printf("    Allocating structures with specific alignment requirements:\n");
    
    /* Allocate 16-byte aligned data for SIMD */
    memento_result_t result16 = memento_alloc_aligned(alloc, sizeof(AlignedData16), 16);
    if (result16.success) {
        AlignedData16* data16 = (AlignedData16*)result16.ptr;
        printf("    16-byte aligned data at %p\n", (void*)data16);
        
        /* Initialize with SIMD-friendly pattern */
        for (int i = 0; i < 16; i++) {
            data16->data[i] = (char)i;
        }
        
        memento_free(alloc, data16);
    }
    
    /* Allocate Vector4 for SSE operations */
    memento_result_t vec_result = memento_alloc_aligned(alloc, sizeof(Vector4), 16);
    if (vec_result.success) {
        Vector4* vec = (Vector4*)vec_result.ptr;
        printf("    16-byte aligned Vector4 at %p\n", (void*)vec);
        
        vec->x = 1.0f; vec->y = 2.0f; vec->z = 3.0f; vec->w = 4.0f;
        printf("      Values: (%.1f, %.1f, %.1f, %.1f)\n", vec->x, vec->y, vec->z, vec->w);
        
        memento_free(alloc, vec);
    }
    
    /* Allocate DoubleVector for AVX operations */
    memento_result_t double_result = memento_alloc_aligned(alloc, sizeof(DoubleVector), 16);
    if (double_result.success) {
        DoubleVector* dvec = (DoubleVector*)double_result.ptr;
        printf("    16-byte aligned DoubleVector at %p\n", (void*)dvec);
        
        dvec->values[0] = 1.0; dvec->values[1] = 2.0;
        printf("      Values: [%.1f, %.1f]\n", dvec->values[0], dvec->values[1]);
        
        memento_free(alloc, dvec);
    }
    
    memento_destroy_allocator(alloc);
    printf("    Structure alignment demo completed\n");
}

static void demonstrate_cache_line_alignment(void) {
    printf("\n  Cache Line Alignment:\n");
    
    memento_allocator_t* alloc = memento_create_thread_cache("align_cache");
    if (!alloc) {
        printf("    Failed to create allocator!\n");
        return;
    }
    
    printf("    Cache line size is typically 64 bytes\n");
    printf("    32-byte alignment for cache-friendly data structures:\n");
    
    /* Allocate multiple cache line aligned structures */
    for (int i = 0; i < 4; i++) {
        memento_result_t result = memento_alloc_aligned(alloc, sizeof(CacheLineData), 32);
        if (result.success) {
            CacheLineData* cache_data = (CacheLineData*)result.ptr;
            printf("    Cache line data %d at %p (32-byte aligned)\n", i, (void*)cache_data);
            
            /* Initialize cache data */
            for (int j = 0; j < 4; j++) {
                cache_data->data[j] = i * 1000 + j;
            }
            
            memento_free(alloc, cache_data);
        }
    }
    
    memento_destroy_allocator(alloc);
    printf("    Cache line alignment demo completed\n");
}

static void demonstrate_block_allocator_alignment(void) {
    printf("\n  Block Allocator Alignment:\n");
    
    memento_allocator_t* backing = memento_create_thread_cache("align_block_back");
    memento_allocator_t* block = memento_create_block_allocator("align_block", backing);
    
    if (!backing || !block) {
        printf("    Failed to create allocators!\n");
        if (backing) memento_destroy_allocator(backing);
        return;
    }
    
    printf("    Block allocator also supports alignment:\n");
    
    /* Test different alignments with block allocator */
    size_t test_alignments[] = {8, 16, 32, 64};
    
    for (size_t i = 0; i < sizeof(test_alignments)/sizeof(test_alignments[0]); i++) {
        size_t alignment = test_alignments[i];
        memento_result_t result = memento_alloc_aligned(block, 512, alignment);
        
        if (result.success) {
            uintptr_t ptr_val = (uintptr_t)result.ptr;
            int is_aligned = (ptr_val % alignment) == 0;
            
            printf("    Block allocator alignment %2zu: %s (aligned: %s)\n", 
                   alignment, result.success ? "SUCCESS" : "FAILED",
                   is_aligned ? "YES" : "NO");
            
            if (result.ptr) {
                memento_free(block, result.ptr);
            }
        }
    }
    
    memento_destroy_allocator(block);
    memento_destroy_allocator(backing);
    printf("    Block allocator alignment demo completed\n");
}

static void demonstrate_alignment_validation(void) {
    printf("\n  Alignment Validation:\n");
    
    memento_allocator_t* alloc = memento_create_thread_cache("align_validate");
    if (!alloc) {
        printf("    Failed to create allocator!\n");
        return;
    }
    
    printf("    Testing alignment validation:\n");
    
    /* Test invalid alignments */
    printf("    Invalid alignments (should fail):\n");
    
    /* Not power of two */
    memento_result_t result = memento_alloc_aligned(alloc, 1024, 3);
    printf("      Alignment 3: %s\n", result.success ? "UNEXPECTED SUCCESS" : "FAILED (correct)");
    
    /* Zero alignment */
    result = memento_alloc_aligned(alloc, 1024, 0);
    printf("      Alignment 0: %s\n", result.success ? "UNEXPECTED SUCCESS" : "FAILED (correct)");
    
    /* Excessive alignment */
    result = memento_alloc_aligned(alloc, 1024, MEMENTO_MAX_ALIGNMENT * 2);
    printf("      Alignment > MAX: %s\n", result.success ? "UNEXPECTED SUCCESS" : "FAILED (correct)");
    
    /* Test valid alignments */
    printf("    Valid alignments (should succeed):\n");
    
    result = memento_alloc_aligned(alloc, 1024, 16);
    printf("      Alignment 16: %s\n", result.success ? "SUCCESS" : "FAILED");
    if (result.ptr) {
        memento_free(alloc, result.ptr);
    }
    
    result = memento_alloc_aligned(alloc, 1024, 64);
    printf("      Alignment 64: %s\n", result.success ? "SUCCESS" : "FAILED");
    if (result.ptr) {
        memento_free(alloc, result.ptr);
    }
    
    memento_destroy_allocator(alloc);
    printf("    Alignment validation demo completed\n");
}

static void demonstrate_performance_comparison(void) {
    printf("\n  Performance Comparison:\n");
    
    memento_allocator_t* alloc = memento_create_thread_cache("align_perf");
    if (!alloc) {
        printf("    Failed to create allocator!\n");
        return;
    }
    
    printf("    Measuring allocation performance with different alignments:\n");
    
    const int iterations = 10000;
    size_t test_alignments[] = {8, 16, 32, 64};
    
    for (size_t i = 0; i < sizeof(test_alignments)/sizeof(test_alignments[0]); i++) {
        size_t alignment = test_alignments[i];
        
        clock_t start = clock();
        
        for (int j = 0; j < iterations; j++) {
            memento_result_t result = memento_alloc_aligned(alloc, 256, alignment);
            if (result.success && result.ptr) {
                memento_free(alloc, result.ptr);
            }
        }
        
        clock_t end = clock();
        double cpu_time = ((double)(end - start)) / CLOCKS_PER_SEC;
        double allocs_per_sec = iterations / cpu_time;
        
        printf("    Alignment %2zu: %.0f allocations/second\n", alignment, allocs_per_sec);
    }
    
    memento_destroy_allocator(alloc);
    printf("    Performance comparison demo completed\n");
}

int main(void) {
    printf("Memento Memory Alignment Example\n");
    printf("================================\n\n");
    
    /* Initialize memento */
    if (memento_init() != MEMENTO_SUCCESS) {
        printf("Failed to initialize memento!\n");
        return 1;
    }
    
    printf("This example demonstrates memento's alignment features:\n");
    printf("- Basic alignment for different sizes\n");
    printf("- Structure alignment for SIMD operations\n");
    printf("- Cache line alignment for performance\n");
    printf("- Block allocator alignment support\n");
    printf("- Alignment validation and error handling\n");
    printf("- Performance impact of different alignments\n\n");
    
    /* Run all demonstrations */
    demonstrate_basic_alignment();
    demonstrate_struct_alignment();
    demonstrate_cache_line_alignment();
    demonstrate_block_allocator_alignment();
    demonstrate_alignment_validation();
    demonstrate_performance_comparison();
    
    printf("\n✓ Memory alignment example completed successfully!\n");
    printf("\nKey takeaways:\n");
    printf("- Memento supports alignments from 1 to %d bytes\n", MEMENTO_MAX_ALIGNMENT);
    printf("- Power-of-two alignments are required\n");
    printf("- Proper alignment is crucial for SIMD and cache performance\n");
    printf("- Block allocator also supports custom alignment\n");
    printf("- Invalid alignments are rejected with clear error handling\n");
    
    memento_shutdown();
    return 0;
}
