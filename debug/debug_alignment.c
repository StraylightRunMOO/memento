#define MEMENTO_IMPLEMENTATION
#include "memento.h"
#include <stdio.h>
#include <stdint.h>

int main() {
    memento_init();
    
    memento_allocator_t* backing = memento_create_thread_cache("debug_back");
    memento_allocator_t* block = memento_create_block_allocator("debug_block", backing);
    
    printf("Testing 16-byte alignment with block allocator\n");
    
    memento_result_t result = memento_alloc_aligned(block, 256, 16);
    if (result.success) {
        uintptr_t ptr_val = (uintptr_t)result.ptr;
        printf("Allocated at: %p\n", result.ptr);
        printf("Pointer value: 0x%lx\n", ptr_val);
        printf("Alignment check: %lu %% 16 = %lu\n", ptr_val, ptr_val % 16);
        
        if (ptr_val % 16 == 0) {
            printf("SUCCESS: Properly aligned!\n");
        } else {
            printf("FAILURE: Not aligned!\n");
        }
        
        memento_free(block, result.ptr);
    } else {
        printf("Allocation failed!\n");
    }
    
    memento_destroy_allocator(block);
    memento_destroy_allocator(backing);
    memento_shutdown();
    
    return 0;
}