#define MEMENTO_IMPLEMENTATION
#include "memento.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main() {
    memento_init();
    
    memento_allocator_t* alloc = memento_create_thread_cache("debug_stress");
    
    printf("=== Stress Test Debug ===\n");
    
    /* Simple test case */
    void* ptrs[5];
    size_t sizes[5];
    int allocated_count = 0;
    
    /* Allocate a few items */
    for (int i = 0; i < 3; i++) {
        size_t size = 64;
        memento_result_t result = memento_alloc(alloc, size);
        if (result.success) {
            int index = allocated_count++;
            ptrs[index] = result.ptr;
            sizes[index] = size;
            
            /* Write pattern */
            unsigned char pattern = index & 0xFF;
            memset(result.ptr, pattern, size);
            
            printf("Allocated item %d: pattern=%d, ptr=%p, size=%zu\n", 
                   index, pattern, result.ptr, size);
        }
    }
    
    /* Try to deallocate one */
    if (allocated_count > 0) {
        int index = 0;  /* Try first one */
        printf("\nTrying to deallocate item %d\n", index);
        printf("Expected pattern: %d\n", index & 0xFF);
        
        unsigned char* bytes = (unsigned char*)ptrs[index];
        printf("First byte: %d\n", bytes[0]);
        printf("Last byte: %d\n", bytes[sizes[index] - 1]);
        
        if (bytes[0] == (index & 0xFF)) {
            printf("Pattern matches!\n");
            memento_free(alloc, ptrs[index]);
            printf("Successfully freed\n");
        } else {
            printf("Pattern mismatch!\n");
        }
    }
    
    memento_destroy_allocator(alloc);
    memento_shutdown();
    
    return 0;
}