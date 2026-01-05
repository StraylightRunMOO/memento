/*
 * Stress tests for memento allocator
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <stdint.h>

#define MEMENTO_IMPLEMENTATION
#include "../memento.h"

#define STRESS_ITERATIONS 100000
#define MAX_ALLOC_SIZE 16384
#define MAX_ALLOCS 1000

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

TEST(random_allocation_pattern) {
    memento_allocator_t* alloc = memento_create_thread_cache("random_stress");
    ASSERT_NOT_NULL(alloc);
    
    srand((unsigned int)time(NULL));
    
    void* ptrs[MAX_ALLOCS] = {0};
    size_t sizes[MAX_ALLOCS] = {0};
    int allocated_count = 0;
    
    for (int i = 0; i < STRESS_ITERATIONS; i++) {
        if (allocated_count < MAX_ALLOCS && (rand() % 2 == 0 || allocated_count == 0)) {
            /* Allocate */
            size_t size = (rand() % MAX_ALLOC_SIZE) + 1;
            memento_result_t result = memento_alloc(alloc, size);
            
            if (result.success) {
                int index = allocated_count++;
                ptrs[index] = result.ptr;
                sizes[index] = size;
                
                /* Write pattern to detect corruption */
                memset(result.ptr, index & 0xFF, size);
            }
        } else if (allocated_count > 0) {
            /* Deallocate */
            int index = rand() % allocated_count;
            
            /* Verify pattern before deallocation */
            unsigned char expected = index & 0xFF;
            unsigned char* bytes = (unsigned char*)ptrs[index];
            for (size_t j = 0; j < sizes[index]; j++) {
                ASSERT(bytes[j] == expected);
            }
            
            memento_free(alloc, ptrs[index]);
            
            /* Move last element to fill gap */
            ptrs[index] = ptrs[allocated_count - 1];
            sizes[index] = sizes[allocated_count - 1];
            allocated_count--;
        }
    }
    
    /* Clean up remaining allocations */
    for (int i = 0; i < allocated_count; i++) {
        unsigned char expected = i & 0xFF;
        unsigned char* bytes = (unsigned char*)ptrs[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            ASSERT(bytes[j] == expected);
        }
        memento_free(alloc, ptrs[i]);
    }
    
    memento_destroy_allocator(alloc);
}

TEST(fragmentation_stress) {
    memento_allocator_t* backing = memento_create_thread_cache("frag_back");
    ASSERT_NOT_NULL(backing);
    
    memento_allocator_t* block = memento_create_block_allocator("frag_block", backing);
    ASSERT_NOT_NULL(block);
    
    /* Create fragmentation pattern */
    void* ptrs[200];
    
    /* Allocate alternating small and large blocks */
    for (int i = 0; i < 200; i++) {
        size_t size = (i % 2 == 0) ? 64 : 1024;
        memento_result_t result = memento_alloc(block, size);
        ASSERT(result.success);
        ptrs[i] = result.ptr;
        memset(result.ptr, i & 0xFF, size);
    }
    
    /* Free every other allocation to create fragmentation */
    for (int i = 0; i < 200; i += 2) {
        memento_free(block, ptrs[i]);
    }
    
    /* Try to allocate in fragmented space */
    for (int i = 0; i < 50; i++) {
        memento_result_t result = memento_alloc(block, 64);
        ASSERT(result.success);  /* Should reuse freed space */
        memset(result.ptr, 0xFF, 64);
        memento_free(block, result.ptr);
    }
    
    /* Clean up remaining */
    for (int i = 1; i < 200; i += 2) {
        memento_free(block, ptrs[i]);
    }
    
    memento_destroy_allocator(block);
    memento_destroy_allocator(backing);
}

TEST(large_allocation_stress) {
    memento_allocator_t* alloc = memento_create_thread_cache("large_stress");
    ASSERT_NOT_NULL(alloc);
    
    /* Test large allocations that may trigger different code paths */
    size_t large_sizes[] = {
        1024 * 1024,     /* 1MB */
        4 * 1024 * 1024, /* 4MB */
        8 * 1024 * 1024, /* 8MB */
        16 * 1024 * 1024 /* 16MB */
    };
    
    for (size_t i = 0; i < sizeof(large_sizes)/sizeof(large_sizes[0]); i++) {
        memento_result_t result = memento_alloc(alloc, large_sizes[i]);
        if (result.success) {
            ASSERT_NOT_NULL(result.ptr);
            
            /* Write pattern to large allocation */
            memset(result.ptr, i & 0xFF, large_sizes[i]);
            
            /* Verify pattern */
            unsigned char* bytes = (unsigned char*)result.ptr;
            for (size_t j = 0; j < large_sizes[i]; j += 4096) {  /* Check every 4KB */
                ASSERT(bytes[j] == (i & 0xFF));
            }
            
            memento_free(alloc, result.ptr);
        }
    }
    
    memento_destroy_allocator(alloc);
}

TEST(rapid_allocation_stress) {
    memento_allocator_t* alloc = memento_create_thread_cache("rapid_stress");
    ASSERT_NOT_NULL(alloc);
    
    /* Rapid small allocations to stress the fast path */
    for (int i = 0; i < 10000; i++) {
        memento_result_t result = memento_alloc(alloc, 64);
        ASSERT(result.success);
        ASSERT_NOT_NULL(result.ptr);
        
        *(int*)result.ptr = i;  /* Simple write to verify memory */
        ASSERT(*(int*)result.ptr == i);
        
        memento_free(alloc, result.ptr);
    }
    
    memento_destroy_allocator(alloc);
}

TEST(mixed_allocator_stress) {
    memento_allocator_t* backing = memento_create_thread_cache("mixed_back");
    ASSERT_NOT_NULL(backing);
    
    memento_allocator_t* thread_cache = memento_create_thread_cache("mixed_thread");
    memento_allocator_t* block = memento_create_block_allocator("mixed_block", backing);
    memento_allocator_t* proxy = memento_create_proxy_allocator("mixed_proxy", backing);
    
    ASSERT_NOT_NULL(thread_cache);
    ASSERT_NOT_NULL(block);
    ASSERT_NOT_NULL(proxy);
    
    /* Use different allocators in sequence */
    for (int i = 0; i < 1000; i++) {
        memento_allocator_t* alloc;
        const char* name;
        
        switch (i % 3) {
            case 0: alloc = thread_cache; name = "thread"; break;
            case 1: alloc = block; name = "block"; break;
            case 2: alloc = proxy; name = "proxy"; break;
        }
        
        size_t size = (i % 10 + 1) * 100;
        memento_result_t result = memento_alloc(alloc, size);
        ASSERT(result.success);
        
        memset(result.ptr, i & 0xFF, size);
        memento_free(alloc, result.ptr);
    }
    
    memento_destroy_allocator(proxy);
    memento_destroy_allocator(block);
    memento_destroy_allocator(thread_cache);
    memento_destroy_allocator(backing);
}

int main(void) {
    printf("Memento Stress Tests\n");
    printf("====================\n");
    printf("Iterations: %d, Max allocation size: %d bytes\n\n", STRESS_ITERATIONS, MAX_ALLOC_SIZE);
    
    if (memento_init() != MEMENTO_SUCCESS) {
        printf("Failed to initialize memento\n");
        return 1;
    }
    
    RUN_TEST(random_allocation_pattern);
    RUN_TEST(fragmentation_stress);
    RUN_TEST(large_allocation_stress);
    RUN_TEST(rapid_allocation_stress);
    RUN_TEST(mixed_allocator_stress);
    
    memento_shutdown();
    
    printf("\n========================================\n");
    printf("Test Summary:\n");
    printf("  Total: %d\n", test_count);
    printf("  Passed: %d\n", pass_count);
    printf("  Failed: %d\n", test_count - pass_count);
    printf("========================================\n");
    
    return (test_count == pass_count) ? 0 : 1;
}