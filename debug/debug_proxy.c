#define MEMENTO_IMPLEMENTATION
#include "memento.h"
#include <stdio.h>

int main() {
    memento_init();
    
    memento_allocator_t* backing = memento_create_thread_cache("debug_back");
    memento_allocator_t* proxy = memento_create_proxy_allocator("debug_proxy", backing);
    
    printf("=== Proxy Allocator Debug ===\n");
    
    const memento_stats_t* proxy_stats = memento_get_stats(proxy);
    const memento_stats_t* backing_stats = memento_get_stats(backing);
    
    printf("Before allocation:\n");
    printf("  Proxy allocations: %zu\n", proxy_stats->allocation_count);
    printf("  Backing allocations: %zu\n", backing_stats->allocation_count);
    
    /* Allocate through proxy */
    memento_result_t result = memento_alloc(proxy, 1024);
    
    proxy_stats = memento_get_stats(proxy);
    backing_stats = memento_get_stats(backing);
    
    printf("\nAfter allocation:\n");
    printf("  Proxy allocations: %zu\n", proxy_stats->allocation_count);
    printf("  Backing allocations: %zu\n", backing_stats->allocation_count);
    printf("  Allocation successful: %s\n", result.success ? "yes" : "no");
    
    if (result.success) {
        memento_free(proxy, result.ptr);
        
        proxy_stats = memento_get_stats(proxy);
        backing_stats = memento_get_stats(backing);
        
        printf("\nAfter deallocation:\n");
        printf("  Proxy deallocations: %zu\n", proxy_stats->deallocation_count);
        printf("  Backing deallocations: %zu\n", backing_stats->deallocation_count);
    }
    
    memento_destroy_allocator(proxy);
    memento_destroy_allocator(backing);
    memento_shutdown();
    
    return 0;
}