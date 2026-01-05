#define MEMENTO_IMPLEMENTATION
#include "memento.h"
#include <stdio.h>
#include <stdint.h>

int main() {
    memento_init();
    
    memento_allocator_t* backing = memento_create_thread_cache("debug_back");
    memento_allocator_t* block = memento_create_block_allocator("debug_block", backing);
    
    printf("=== Block Allocator Alignment Debug ===\n");
    printf("Block size: %d bytes\n", MEMENTO_BLOCK_SIZE);
    printf("Block header size: %zu bytes\n", sizeof(memento_block_header_t));
    printf("Chunk header size: %zu bytes\n", sizeof(memento_block_chunk_t));
    printf("MEMENTO_CHUNK_ALIGNMENT: %d bytes\n", MEMENTO_CHUNK_ALIGNMENT);
    printf("\n");
    
    /* Test with different alignments */
    size_t test_alignments[] = {8, 16, 32, 64};
    
    for (int i = 0; i < 4; i++) {
        size_t alignment = test_alignments[i];
        printf("Testing alignment: %zu bytes\n", alignment);
        
        memento_result_t result = memento_alloc_aligned(block, 256, alignment);
        if (result.success) {
            uintptr_t ptr_val = (uintptr_t)result.ptr;
            printf("  Allocated at: %p\n", result.ptr);
            printf("  Pointer value: 0x%lx\n", ptr_val);
            printf("  Alignment check: %lu %% %zu = %lu\n", ptr_val, alignment, ptr_val % alignment);
            printf("  Result: %s\n\n", (ptr_val % alignment == 0) ? "ALIGNED" : "NOT ALIGNED");
            
            memento_free(block, result.ptr);
        } else {
            printf("  ALLOCATION FAILED!\n\n");
        }
    }
    
    memento_destroy_allocator(block);
    memento_destroy_allocator(backing);
    memento_shutdown();
    
    return 0;
}