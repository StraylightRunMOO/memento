/*
 * Alignment-specific tests for memento allocator
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

#define MEMENTO_IMPLEMENTATION
#include "../memento.h"

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

#define ASSERT_ALIGNED(ptr, alignment) do { \
    uintptr_t val = (uintptr_t)(ptr); \
    if ((val % (alignment)) != 0) { \
        printf("FAIL\n"); \
        printf("    Pointer %p not aligned to %zu bytes\n", (ptr), (size_t)(alignment)); \
        printf("    at %s:%d\n", __FILE__, __LINE__); \
        exit(1); \
    } \
} while(0)

TEST(power_of_two_alignment) {
    memento_allocator_t* alloc = memento_create_thread_cache("pow2_align");
    ASSERT_NOT_NULL(alloc);
    
    size_t alignments[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    
    for (size_t i = 0; i < sizeof(alignments)/sizeof(alignments[0]); i++) {
        size_t alignment = alignments[i];
        memento_result_t result = memento_alloc_aligned(alloc, 100, alignment);
        ASSERT(result.success);
        ASSERT_NOT_NULL(result.ptr);
        ASSERT_ALIGNED(result.ptr, alignment);
        
        memset(result.ptr, 0xAB, 100);
        memento_free(alloc, result.ptr);
    }
    
    memento_destroy_allocator(alloc);
}

TEST(large_alignment) {
    memento_allocator_t* alloc = memento_create_thread_cache("large_align");
    ASSERT_NOT_NULL(alloc);
    
    size_t large_alignments[] = {1024, 2048, 4096, 8192, 16384};
    
    for (size_t i = 0; i < sizeof(large_alignments)/sizeof(large_alignments[0]); i++) {
        size_t alignment = large_alignments[i];
        memento_result_t result = memento_alloc_aligned(alloc, 256, alignment);
        ASSERT(result.success);
        ASSERT_NOT_NULL(result.ptr);
        ASSERT_ALIGNED(result.ptr, alignment);
        
        memento_free(alloc, result.ptr);
    }
    
    memento_destroy_allocator(alloc);
}

TEST(cache_line_alignment) {
    memento_allocator_t* alloc = memento_create_thread_cache("cache_align");
    ASSERT_NOT_NULL(alloc);
    
    size_t cache_line_sizes[] = {32, 64, 128};  /* Common cache line sizes */
    
    for (size_t i = 0; i < sizeof(cache_line_sizes)/sizeof(cache_line_sizes[0]); i++) {
        size_t alignment = cache_line_sizes[i];
        memento_result_t result = memento_alloc_aligned(alloc, 64, alignment);
        ASSERT(result.success);
        ASSERT_NOT_NULL(result.ptr);
        ASSERT_ALIGNED(result.ptr, alignment);
        
        /* Write pattern to verify memory is accessible */
        memset(result.ptr, 0xCD, 64);
        
        memento_free(alloc, result.ptr);
    }
    
    memento_destroy_allocator(alloc);
}

TEST(struct_alignment) {
    memento_allocator_t* alloc = memento_create_thread_cache("struct_align");
    ASSERT_NOT_NULL(alloc);
    
    /* Test alignment for different struct types */
    struct SmallStruct { char c; };
    struct MediumStruct { int i; double d; };
    struct LargeStruct { char c; int i; double d; long l; void* ptr; };
    
    /* Test common alignments for different struct sizes */
    struct {
        const char* name;
        size_t size;
        size_t alignment;
    } structs[] = {
        {"SmallStruct", sizeof(struct SmallStruct), 1},
        {"MediumStruct", sizeof(struct MediumStruct), 8},
        {"LargeStruct", sizeof(struct LargeStruct), 8},
    };
    
    for (size_t i = 0; i < sizeof(structs)/sizeof(structs[0]); i++) {
        memento_result_t result = memento_alloc_aligned(alloc, structs[i].size, structs[i].alignment);
        ASSERT(result.success);
        ASSERT_NOT_NULL(result.ptr);
        ASSERT_ALIGNED(result.ptr, structs[i].alignment);
        
        memset(result.ptr, 0, structs[i].size);
        memento_free(alloc, result.ptr);
    }
    
    memento_destroy_allocator(alloc);
}

TEST(maximum_alignment) {
    memento_allocator_t* alloc = memento_create_thread_cache("max_align");
    ASSERT_NOT_NULL(alloc);
    
    /* Test maximum supported alignment */
    size_t max_alignment = MEMENTO_MAX_ALIGNMENT;
    memento_result_t result = memento_alloc_aligned(alloc, 1024, max_alignment);
    ASSERT(result.success);
    ASSERT_NOT_NULL(result.ptr);
    ASSERT_ALIGNED(result.ptr, max_alignment);
    
    memento_free(alloc, result.ptr);
    memento_destroy_allocator(alloc);
}

TEST(block_allocator_alignment) {
    memento_allocator_t* backing = memento_create_thread_cache("block_back_align");
    ASSERT_NOT_NULL(backing);
    
    memento_allocator_t* block = memento_create_block_allocator("block_align", backing);
    ASSERT_NOT_NULL(block);
    
    /* Test alignment with block allocator */
    size_t alignments[] = {16, 32, 64, 128, 256};
    
    for (size_t i = 0; i < sizeof(alignments)/sizeof(alignments[0]); i++) {
        size_t alignment = alignments[i];
        memento_result_t result = memento_alloc_aligned(block, 512, alignment);
        ASSERT(result.success);
        ASSERT_NOT_NULL(result.ptr);
        ASSERT_ALIGNED(result.ptr, alignment);
        
        memento_free(block, result.ptr);
    }
    
    memento_destroy_allocator(block);
    memento_destroy_allocator(backing);
}

TEST(stack_allocator_alignment) {
    memento_allocator_t* backing = memento_create_thread_cache("stack_back_align");
    ASSERT_NOT_NULL(backing);
    
    memento_allocator_t* stack = memento_create_stack_allocator("stack_align", 64 * 1024, backing);
    ASSERT_NOT_NULL(stack);
    
    /* Test alignment with stack allocator */
    size_t alignments[] = {8, 16, 32, 64};
    
    for (size_t i = 0; i < sizeof(alignments)/sizeof(alignments[0]); i++) {
        size_t alignment = alignments[i];
        memento_result_t result = memento_alloc_aligned(stack, 256, alignment);
        ASSERT(result.success);
        ASSERT_NOT_NULL(result.ptr);
        ASSERT_ALIGNED(result.ptr, alignment);
        
        /* Stack allocator doesn't support individual deallocation */
    }
    
    memento_destroy_allocator(stack);
    memento_destroy_allocator(backing);
}

int main(void) {
    printf("Memento Alignment Tests\n");
    printf("=======================\n");
    
    if (memento_init() != MEMENTO_SUCCESS) {
        printf("Failed to initialize memento\n");
        return 1;
    }
    
    RUN_TEST(power_of_two_alignment);
    RUN_TEST(large_alignment);
    RUN_TEST(cache_line_alignment);
    RUN_TEST(struct_alignment);
    RUN_TEST(maximum_alignment);
    RUN_TEST(block_allocator_alignment);
    RUN_TEST(stack_allocator_alignment);
    
    memento_shutdown();
    
    printf("\n========================================\n");
    printf("Test Summary:\n");
    printf("  Total: %d\n", test_count);
    printf("  Passed: %d\n", pass_count);
    printf("  Failed: %d\n", test_count - pass_count);
    printf("========================================\n");
    
    return (test_count == pass_count) ? 0 : 1;
}