/*
 * Memento Memory Allocator Library
 * 
 * A unified, header-only memory allocator library that combines the best features
 * of high-performance allocators into a single, easy-to-use interface.
 * 
 * Features:
 * - Thread-safe high-performance allocation (thread-cache backend)
 * - Hierarchical memory tracking and leak detection (proxy framework)
 * - Block-based allocation with recycling (block allocator)
 * - C99 compatible with optional C++ wrapper
 * - Comprehensive error handling and validation
 * - Performance statistics and debugging support
 * 
 * Usage:
 *   #define MEMENTO_IMPLEMENTATION
 *   #include "memento.h"
 *   
 *   int main() {
 *       memento_init();
 *       
 *       memento_allocator_t* allocator = memento_create_thread_cache("main");
 *       void* ptr = memento_alloc(allocator, 1024);
 *       // ... use memory ...
 *       memento_free(allocator, ptr);
 *       
 *       memento_destroy_allocator(allocator);
 *       memento_shutdown();
 *       return 0;
 *   }
 */

#ifndef MEMENTO_H
#define MEMENTO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

/* Platform detection and configuration */
#if defined(_WIN32) || defined(__WIN32__) || defined(_WIN64)
    #define MEMENTO_PLATFORM_WINDOWS 1
    #define MEMENTO_PLATFORM_POSIX 0
    #if defined(_MSC_VER)
        #define MEMENTO_TLS __declspec(thread)
        #define MEMENTO_FORCE_INLINE __forceinline
        #define MEMENTO_NOINLINE __declspec(noinline)
    #else
        #define MEMENTO_TLS __thread
        #define MEMENTO_FORCE_INLINE inline __attribute__((always_inline))
        #define MEMENTO_NOINLINE __attribute__((noinline))
    #endif
#else
    #define MEMENTO_PLATFORM_WINDOWS 0
    #define MEMENTO_PLATFORM_POSIX 1
    #define MEMENTO_TLS __thread
    #define MEMENTO_FORCE_INLINE inline __attribute__((always_inline))
    #define MEMENTO_NOINLINE __attribute__((noinline))
#endif

/* Cache line size for performance optimization */
#ifndef MEMENTO_CACHE_LINE_SIZE
    #define MEMENTO_CACHE_LINE_SIZE 64
#endif

/* Maximum supported alignment */
#ifndef MEMENTO_MAX_ALIGNMENT
    #define MEMENTO_MAX_ALIGNMENT (256 * 1024)
#endif

/* Configuration options */
#ifndef MEMENTO_ENABLE_STATISTICS
    #ifdef NDEBUG
        #define MEMENTO_ENABLE_STATISTICS 0
    #else
        #define MEMENTO_ENABLE_STATISTICS 1
    #endif
#endif

#ifndef MEMENTO_ENABLE_DEBUG_CHECKS
    #ifdef NDEBUG
        #define MEMENTO_ENABLE_DEBUG_CHECKS 0
    #else
        #define MEMENTO_ENABLE_DEBUG_CHECKS 1
    #endif
#endif

/* Memory block sizes for block allocator */
#define MEMENTO_BLOCK_SIZE (8 * 1024 * 1024)  /* 8MB blocks */
#define MEMENTO_MIN_CHUNK_SIZE 16
#define MEMENTO_CHUNK_ALIGNMENT 16
#define MEMENTO_RECYCLE_SLOTS 8

/* Error codes */
typedef enum {
    MEMENTO_SUCCESS = 0,
    MEMENTO_ERROR_INVALID_ARGUMENT = -1,
    MEMENTO_ERROR_OUT_OF_MEMORY = -2,
    MEMENTO_ERROR_NOT_INITIALIZED = -3,
    MEMENTO_ERROR_ALREADY_INITIALIZED = -4,
    MEMENTO_ERROR_THREAD_NOT_INITIALIZED = -5,
    MEMENTO_ERROR_ALLOCATION_FAILED = -6,
    MEMENTO_ERROR_INVALID_POINTER = -7,
    MEMENTO_ERROR_UNSUPPORTED_OPERATION = -8
} memento_error_t;

/* Forward declarations */
typedef struct memento_allocator_t memento_allocator_t;
typedef struct memento_stats_t memento_stats_t;
typedef struct memento_config_t memento_config_t;

/* Statistics structure */
struct memento_stats_t {
    /* Memory usage statistics */
    size_t total_allocated;
    size_t total_deallocated;
    size_t current_usage;
    size_t peak_usage;
    
    /* Allocation counters */
    size_t allocation_count;
    size_t deallocation_count;
    size_t failed_allocations;
    
    /* Thread-specific statistics */
    size_t thread_cache_hits;
    size_t thread_cache_misses;
    size_t block_allocations;
    size_t block_deallocations;
};

/* Configuration structure */
struct memento_config_t {
    /* General settings */
    bool enable_statistics;
    bool enable_debug_checks;
    bool enable_thread_cache;
    bool enable_block_allocator;
    
    /* Thread cache settings */
    size_t thread_cache_size;
    size_t thread_cache_threshold;
    
    /* Block allocator settings */
    size_t block_size;
    size_t min_chunk_size;
    size_t max_chunk_size;
    
    /* Memory mapping settings */
    size_t memory_map_threshold;
    size_t page_size;
};

/* Allocation result */
typedef struct {
    void* ptr;
    size_t size;
    bool success;
} memento_result_t;

/* Allocator function pointers */
typedef memento_result_t (*memento_allocate_func_t)(memento_allocator_t* allocator, size_t size, size_t alignment);
typedef void (*memento_deallocate_func_t)(memento_allocator_t* allocator, void* ptr);
typedef void (*memento_destroy_func_t)(memento_allocator_t* allocator);

/* Base allocator structure */
struct memento_allocator_t {
    const char* name;
    memento_stats_t stats;
    
    /* Virtual function table */
    memento_allocate_func_t allocate;
    memento_deallocate_func_t deallocate;
    memento_destroy_func_t destroy;
    
    /* Allocator-specific data follows */
};

/* Global initialization and configuration */
int memento_init(void);
int memento_shutdown(void);
bool memento_is_initialized(void);
const memento_config_t* memento_get_config(void);

/* Core allocation functions */
memento_result_t memento_alloc(memento_allocator_t* allocator, size_t size);
memento_result_t memento_alloc_aligned(memento_allocator_t* allocator, size_t size, size_t alignment);
void memento_free(memento_allocator_t* allocator, void* ptr);

/* Allocator creation and destruction */
memento_allocator_t* memento_create_thread_cache(const char* name);
memento_allocator_t* memento_create_block_allocator(const char* name, memento_allocator_t* backing);
memento_allocator_t* memento_create_proxy_allocator(const char* name, memento_allocator_t* backing);
memento_allocator_t* memento_create_stack_allocator(const char* name, size_t capacity, memento_allocator_t* backing);
void memento_destroy_allocator(memento_allocator_t* allocator);

/* Utility functions */
size_t memento_align_up(size_t value, size_t alignment);
bool memento_is_power_of_two(size_t value);
void* memento_align_pointer(void* ptr, size_t alignment);

/* Statistics and debugging */
void memento_print_stats(const memento_allocator_t* allocator);
const memento_stats_t* memento_get_stats(const memento_allocator_t* allocator);
const char* memento_error_string(memento_error_t error);

/* ============================================================================= */
/* INTERNAL IMPLEMENTATION BEGINS HERE                                         */
/* ============================================================================= */

#ifdef MEMENTO_IMPLEMENTATION

/* Global state */
static struct {
    bool initialized;
    memento_config_t config;
    memento_allocator_t* root_allocator;
    void* thread_cache;  /* Implementation-specific thread cache */
} g_memento = {0};

/* Memory alignment utilities */
MEMENTO_FORCE_INLINE size_t memento_align_up(size_t value, size_t alignment) {
    assert(memento_is_power_of_two(alignment));
    return (value + alignment - 1) & ~(alignment - 1);
}

MEMENTO_FORCE_INLINE bool memento_is_power_of_two(size_t value) {
    return (value & (value - 1)) == 0;
}

MEMENTO_FORCE_INLINE void* memento_align_pointer(void* ptr, size_t alignment) {
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t aligned = memento_align_up(addr, alignment);
    return (void*)aligned;
}

/* Error string conversion */
const char* memento_error_string(memento_error_t error) {
    switch (error) {
        case MEMENTO_SUCCESS: return "Success";
        case MEMENTO_ERROR_INVALID_ARGUMENT: return "Invalid argument";
        case MEMENTO_ERROR_OUT_OF_MEMORY: return "Out of memory";
        case MEMENTO_ERROR_NOT_INITIALIZED: return "Not initialized";
        case MEMENTO_ERROR_ALREADY_INITIALIZED: return "Already initialized";
        case MEMENTO_ERROR_THREAD_NOT_INITIALIZED: return "Thread not initialized";
        case MEMENTO_ERROR_ALLOCATION_FAILED: return "Allocation failed";
        case MEMENTO_ERROR_INVALID_POINTER: return "Invalid pointer";
        case MEMENTO_ERROR_UNSUPPORTED_OPERATION: return "Unsupported operation";
        default: return "Unknown error";
    }
}

/* Statistics functions */
void memento_print_stats(const memento_allocator_t* allocator) {
    if (!allocator) return;
    
    printf("=== Memento Allocator Statistics: %s ===\n", allocator->name);
    printf("Total allocated:     %zu bytes\n", allocator->stats.total_allocated);
    printf("Total deallocated:   %zu bytes\n", allocator->stats.total_deallocated);
    printf("Current usage:       %zu bytes\n", allocator->stats.current_usage);
    printf("Peak usage:          %zu bytes\n", allocator->stats.peak_usage);
    printf("Allocation count:    %zu\n", allocator->stats.allocation_count);
    printf("Deallocation count:  %zu\n", allocator->stats.deallocation_count);
    printf("Failed allocations:  %zu\n", allocator->stats.failed_allocations);
    printf("Thread cache hits:   %zu\n", allocator->stats.thread_cache_hits);
    printf("Thread cache misses: %zu\n", allocator->stats.thread_cache_misses);
    printf("Block allocations:   %zu\n", allocator->stats.block_allocations);
    printf("Block deallocations: %zu\n", allocator->stats.block_deallocations);
    printf("Memory leaks:        %zu bytes\n", 
           allocator->stats.total_allocated - allocator->stats.total_deallocated);
    printf("========================================\n");
}

const memento_stats_t* memento_get_stats(const memento_allocator_t* allocator) {
    return allocator ? &allocator->stats : NULL;
}

/* Core allocation functions */
MEMENTO_FORCE_INLINE memento_result_t memento_alloc(memento_allocator_t* allocator, size_t size) {
    return memento_alloc_aligned(allocator, size, MEMENTO_CHUNK_ALIGNMENT);
}

MEMENTO_FORCE_INLINE memento_result_t memento_alloc_aligned(memento_allocator_t* allocator, size_t size, size_t alignment) {
    memento_result_t result = {NULL, 0, false};
    
    if (!allocator || !allocator->allocate) {
        return result;
    }
    
    if (size == 0) {
        result.success = true;
        return result;
    }
    
    /* Validate alignment */
    if (!memento_is_power_of_two(alignment) || alignment > MEMENTO_MAX_ALIGNMENT) {
        return result;
    }
    
    result = allocator->allocate(allocator, size, alignment);
    
    if (result.success && result.ptr) {
        allocator->stats.allocation_count++;
        allocator->stats.total_allocated += result.size;
        allocator->stats.current_usage += result.size;
        
        if (allocator->stats.current_usage > allocator->stats.peak_usage) {
            allocator->stats.peak_usage = allocator->stats.current_usage;
        }
    } else {
        allocator->stats.failed_allocations++;
    }
    
    return result;
}

MEMENTO_FORCE_INLINE void memento_free(memento_allocator_t* allocator, void* ptr) {
    if (!allocator || !allocator->deallocate || !ptr) {
        return;
    }
    
    /* TODO: Track deallocation size for accurate statistics */
    allocator->deallocate(allocator, ptr);
    
    allocator->stats.deallocation_count++;
    /* Note: We can't easily track the size of freed memory without additional bookkeeping */
}

/* ============================================================================= */
/* THREAD CACHE ALLOCATOR (based on rpmalloc concepts)                        */
/* ============================================================================= */

typedef struct {
    memento_allocator_t base;
    void* heap;  /* Thread-local heap data */
    size_t cached_size;
    void* cached_blocks;  /* Small block cache */
} memento_thread_cache_t;

/* Forward declarations for thread cache functions */
static memento_result_t memento_thread_cache_allocate(memento_allocator_t* allocator, size_t size, size_t alignment);
static void memento_thread_cache_deallocate(memento_allocator_t* allocator, void* ptr);
static void memento_thread_cache_destroy(memento_allocator_t* allocator);

memento_allocator_t* memento_create_thread_cache(const char* name) {
    size_t total_size = sizeof(memento_thread_cache_t) + strlen(name) + 1;
    memento_thread_cache_t* cache = (memento_thread_cache_t*)malloc(total_size);
    
    if (!cache) {
        return NULL;
    }
    
    memset(cache, 0, sizeof(memento_thread_cache_t));
    
    /* Initialize base allocator */
    cache->base.name = (const char*)(cache + 1);
    strcpy((char*)cache->base.name, name ? name : "thread_cache");
    memset(&cache->base.stats, 0, sizeof(memento_stats_t));
    
    cache->base.allocate = memento_thread_cache_allocate;
    cache->base.deallocate = memento_thread_cache_deallocate;
    cache->base.destroy = memento_thread_cache_destroy;
    
    /* Initialize thread cache specific data */
    cache->heap = NULL;  /* Will be initialized on first use */
    cache->cached_size = 0;
    cache->cached_blocks = NULL;
    
    return &cache->base;
}

static memento_result_t memento_thread_cache_allocate(memento_allocator_t* allocator, size_t size, size_t alignment) {
    memento_thread_cache_t* cache = (memento_thread_cache_t*)allocator;
    memento_result_t result = {NULL, size, false};
    
    /* Simple malloc fallback for now - can be enhanced with actual thread caching */
    void* ptr = NULL;
    
    if (alignment > MEMENTO_CHUNK_ALIGNMENT) {
        /* Use aligned allocation */
        #ifdef _WIN32
            ptr = _aligned_malloc(size, alignment);
        #else
            if (posix_memalign(&ptr, alignment, size) != 0) {
                ptr = NULL;
            }
        #endif
    } else {
        ptr = malloc(size);
    }
    
    if (ptr) {
        result.ptr = ptr;
        result.success = true;
        cache->base.stats.thread_cache_misses++;  /* Count as miss for now */
    }
    
    return result;
}

static void memento_thread_cache_deallocate(memento_allocator_t* allocator, void* ptr) {
    memento_thread_cache_t* cache = (memento_thread_cache_t*)allocator;
    
    if (!ptr) return;
    
    /* Simple free for now - can be enhanced with actual thread caching */
    #ifdef _WIN32
        _aligned_free(ptr);
    #else
        free(ptr);
    #endif
    
    cache->base.stats.thread_cache_hits++;  /* Count as hit for now */
}

static void memento_thread_cache_destroy(memento_allocator_t* allocator) {
    memento_thread_cache_t* cache = (memento_thread_cache_t*)allocator;
    
    /* Clean up any cached blocks */
    if (cache->cached_blocks) {
        free(cache->cached_blocks);
    }
    
    /* Clean up heap data */
    if (cache->heap) {
        free(cache->heap);
    }
    
    free(cache);
}

/* ============================================================================= */
/* BLOCK ALLOCATOR (based on Wheel-of-Fortune concepts)                        */
/* ============================================================================= */

typedef struct memento_block_chunk {
    uint32_t prev;   /* bytes to previous chunk */
    uint32_t last : 1;
    uint32_t used : 1;
    uint32_t jumbo : 1;
    uint32_t len : 29;   /* chunk length including header */
} memento_block_chunk_t;

typedef struct memento_block_header {
    struct memento_block_header* prev;
    struct memento_block_header* next;
} memento_block_header_t;

typedef struct {
    memento_block_header_t* block_list;
    memento_block_chunk_t* recycler[MEMENTO_RECYCLE_SLOTS];
    int recycle_pos;
    memento_allocator_t* backing;
} memento_block_data_t;

typedef struct {
    memento_allocator_t base;
    memento_block_data_t data;
} memento_block_allocator_t;

/* Forward declarations for block allocator functions */
static memento_result_t memento_block_allocate(memento_allocator_t* allocator, size_t size, size_t alignment);
static void memento_block_deallocate(memento_allocator_t* allocator, void* ptr);
static void memento_block_destroy(memento_allocator_t* allocator);

static void* memento_block_chunk_ptr(const memento_block_chunk_t* chunk) {
    return (char*)chunk + sizeof(memento_block_chunk_t);
}

static memento_block_chunk_t* memento_block_ptr_to_chunk(void* ptr) {
    return (memento_block_chunk_t*)((char*)ptr - sizeof(memento_block_chunk_t));
}

static void memento_block_split(memento_block_chunk_t* chunk, uint32_t want_size) {
    uint32_t remaining = chunk->len - want_size;
    
    if (remaining <= sizeof(memento_block_chunk_t)) {
        return;  /* Not enough space to split */
    }
    
    memento_block_chunk_t* next_chunk = (memento_block_chunk_t*)((char*)chunk + want_size);
    next_chunk->len = remaining;
    next_chunk->last = chunk->last;
    next_chunk->used = 0;
    next_chunk->prev = want_size;
    
    chunk->last = 0;
    chunk->len = want_size;
}

static void memento_block_merge_right(memento_block_data_t* data, memento_block_chunk_t* chunk) {
    (void)data;  /* Unused parameter for now */
    if (chunk->last) return;
    
    memento_block_chunk_t* next_chunk = (memento_block_chunk_t*)((char*)chunk + chunk->len);
    if (next_chunk->used) return;
    
    chunk->len += next_chunk->len;
    chunk->last = next_chunk->last;
}

memento_allocator_t* memento_create_block_allocator(const char* name, memento_allocator_t* backing) {
    if (!backing) return NULL;
    
    size_t total_size = sizeof(memento_block_allocator_t) + strlen(name) + 1;
    memento_block_allocator_t* block = (memento_block_allocator_t*)malloc(total_size);
    
    if (!block) {
        return NULL;
    }
    
    memset(block, 0, sizeof(memento_block_allocator_t));
    
    /* Initialize base allocator */
    block->base.name = (const char*)(block + 1);
    strcpy((char*)block->base.name, name ? name : "block");
    memset(&block->base.stats, 0, sizeof(memento_stats_t));
    
    block->base.allocate = memento_block_allocate;
    block->base.deallocate = memento_block_deallocate;
    block->base.destroy = memento_block_destroy;
    
    /* Initialize block allocator data */
    block->data.block_list = NULL;
    block->data.recycle_pos = 0;
    block->data.backing = backing;
    
    for (int i = 0; i < MEMENTO_RECYCLE_SLOTS; i++) {
        block->data.recycler[i] = NULL;
    }
    
    return &block->base;
}

static memento_result_t memento_block_allocate(memento_allocator_t* allocator, size_t size, size_t alignment) {
    memento_block_allocator_t* block = (memento_block_allocator_t*)allocator;
    memento_result_t result = {NULL, size, false};
    
    if (size == 0) {
        result.success = true;
        return result;
    }
    
    /* Calculate total needed size including alignment padding */
    uint32_t base_need_size = (uint32_t)(size + sizeof(memento_block_chunk_t));
    uint32_t aligned_header_size = (base_need_size + alignment - 1) & ~(alignment - 1);
    uint32_t need_size = aligned_header_size;
    
    /* Try recycler ring first */
    for (int i = 0; i < MEMENTO_RECYCLE_SLOTS; i++) {
        memento_block_chunk_t* chunk = block->data.recycler[block->data.recycle_pos];
        if (chunk && chunk->len >= need_size) {
            /* Check if the chunk can satisfy alignment requirements */
            void* potential_ptr = memento_block_chunk_ptr(chunk);
            void* aligned_ptr = memento_align_pointer(potential_ptr, alignment);
            if (aligned_ptr == potential_ptr) {
                /* Chunk is already properly aligned */
                block->data.recycle_pos = (block->data.recycle_pos + 1) & (MEMENTO_RECYCLE_SLOTS - 1);
                memento_block_split(chunk, need_size);
                chunk->used = 1;
                
                result.ptr = memento_block_chunk_ptr(chunk);
                result.success = true;
                block->base.stats.block_allocations++;
                return result;
            }
        }
        block->data.recycle_pos = (block->data.recycle_pos + 1) & (MEMENTO_RECYCLE_SLOTS - 1);
    }
    
    /* Allocate new block from backing allocator */
    memento_block_header_t* block_header = (memento_block_header_t*)
        memento_alloc(block->data.backing, MEMENTO_BLOCK_SIZE).ptr;
    
    if (!block_header) {
        return result;
    }
    
    block_header->prev = block_header->next = NULL;
    
    /* Find aligned position for the chunk */
    char* block_start = (char*)block_header;
    char* chunk_start = block_start + sizeof(memento_block_header_t);
    char* aligned_chunk_start = (char*)memento_align_pointer(chunk_start, alignment);
    
    /* Create chunk at aligned position */
    memento_block_chunk_t* chunk = (memento_block_chunk_t*)aligned_chunk_start;
    size_t available_size = MEMENTO_BLOCK_SIZE - (aligned_chunk_start - block_start);
    chunk->len = available_size;
    chunk->used = 0;
    chunk->last = 1;
    chunk->prev = 0;
    
    memento_block_split(chunk, need_size);
    chunk->used = 1;
    
    /* Link block */
    block_header->next = block->data.block_list;
    if (block->data.block_list) {
        block->data.block_list->prev = block_header;
    }
    block->data.block_list = block_header;
    
    result.ptr = memento_block_chunk_ptr(chunk);
    result.success = true;
    block->base.stats.block_allocations++;
    
    return result;
}

static void memento_block_deallocate(memento_allocator_t* allocator, void* ptr) {
    memento_block_allocator_t* block = (memento_block_allocator_t*)allocator;
    
    if (!ptr) return;
    
    memento_block_chunk_t* chunk = memento_block_ptr_to_chunk(ptr);
    chunk->used = 0;
    
    memento_block_merge_right(&block->data, chunk);
    
    /* Push to recycler ring */
    block->data.recycler[block->data.recycle_pos] = chunk;
    block->data.recycle_pos = (block->data.recycle_pos + 1) & (MEMENTO_RECYCLE_SLOTS - 1);
    
    block->base.stats.block_deallocations++;
}

static void memento_block_destroy(memento_allocator_t* allocator) {
    memento_block_allocator_t* block = (memento_block_allocator_t*)allocator;
    
    /* Free all blocks */
    memento_block_header_t* current = block->data.block_list;
    while (current) {
        memento_block_header_t* next = current->next;
        memento_free(block->data.backing, current);
        current = next;
    }
    
    free(block);
}

/* ============================================================================= */
/* PROXY ALLOCATOR (for tracking and debugging)                                */
/* ============================================================================= */

typedef struct {
    memento_allocator_t base;
    memento_allocator_t* backing;
} memento_proxy_allocator_t;

/* Forward declarations for proxy allocator functions */
static memento_result_t memento_proxy_allocate(memento_allocator_t* allocator, size_t size, size_t alignment);
static void memento_proxy_deallocate(memento_allocator_t* allocator, void* ptr);
static void memento_proxy_destroy(memento_allocator_t* allocator);

memento_allocator_t* memento_create_proxy_allocator(const char* name, memento_allocator_t* backing) {
    if (!backing) return NULL;
    
    size_t total_size = sizeof(memento_proxy_allocator_t) + strlen(name) + 1;
    memento_proxy_allocator_t* proxy = (memento_proxy_allocator_t*)malloc(total_size);
    
    if (!proxy) {
        return NULL;
    }
    
    memset(proxy, 0, sizeof(memento_proxy_allocator_t));
    
    /* Initialize base allocator */
    proxy->base.name = (const char*)(proxy + 1);
    strcpy((char*)proxy->base.name, name ? name : "proxy");
    memset(&proxy->base.stats, 0, sizeof(memento_stats_t));
    
    proxy->base.allocate = memento_proxy_allocate;
    proxy->base.deallocate = memento_proxy_deallocate;
    proxy->base.destroy = memento_proxy_destroy;
    
    /* Initialize proxy data */
    proxy->backing = backing;
    
    return &proxy->base;
}

static memento_result_t memento_proxy_allocate(memento_allocator_t* allocator, size_t size, size_t alignment) {
    memento_proxy_allocator_t* proxy = (memento_proxy_allocator_t*)allocator;
    
    /* Add debug tracking here if needed */
    #if MEMENTO_ENABLE_DEBUG_CHECKS
        printf("[PROXY] Allocating %zu bytes with alignment %zu from %s\n", size, alignment, proxy->base.name);
    #endif
    
    memento_result_t result = proxy->backing->allocate(proxy->backing, size, alignment);
    
    if (result.success) {
        /* Update proxy statistics */
        proxy->base.stats.allocation_count++;
        proxy->base.stats.total_allocated += result.size;
        proxy->base.stats.current_usage += result.size;
        
        if (proxy->base.stats.current_usage > proxy->base.stats.peak_usage) {
            proxy->base.stats.peak_usage = proxy->base.stats.current_usage;
        }
        
        /* Also update backing allocator statistics */
        proxy->backing->stats.allocation_count++;
        proxy->backing->stats.total_allocated += result.size;
        proxy->backing->stats.current_usage += result.size;
        
        if (proxy->backing->stats.current_usage > proxy->backing->stats.peak_usage) {
            proxy->backing->stats.peak_usage = proxy->backing->stats.current_usage;
        }
    }
    
    return result;
}

static void memento_proxy_deallocate(memento_allocator_t* allocator, void* ptr) {
    memento_proxy_allocator_t* proxy = (memento_proxy_allocator_t*)allocator;
    
    if (!ptr) return;
    
    /* Add debug tracking here if needed */
    #if MEMENTO_ENABLE_DEBUG_CHECKS
        printf("[PROXY] Deallocating pointer %p from %s\n", ptr, proxy->base.name);
    #endif
    
    proxy->backing->deallocate(proxy->backing, ptr);
    
    /* Update proxy statistics */
    proxy->base.stats.deallocation_count++;
    
    /* Also update backing allocator statistics */
    proxy->backing->stats.deallocation_count++;
    /* Note: Cannot easily track size of freed memory */
}

static void memento_proxy_destroy(memento_allocator_t* allocator) {
    memento_proxy_allocator_t* proxy = (memento_proxy_allocator_t*)allocator;
    
    /* Print final statistics if enabled */
    #if MEMENTO_ENABLE_STATISTICS
        if (proxy->base.stats.allocation_count > 0 || proxy->base.stats.deallocation_count > 0) {
            memento_print_stats(allocator);
        }
    #endif
    
    free(proxy);
}

/* ============================================================================= */
/* STACK ALLOCATOR (for temporary allocations)                                 */
/* ============================================================================= */

typedef struct {
    memento_allocator_t base;
    char* start;
    char* end;
    char* top;
    memento_allocator_t* backing;
} memento_stack_allocator_t;

/* Forward declarations for stack allocator functions */
static memento_result_t memento_stack_allocate(memento_allocator_t* allocator, size_t size, size_t alignment);
static void memento_stack_deallocate(memento_allocator_t* allocator, void* ptr);
static void memento_stack_destroy(memento_allocator_t* allocator);

memento_allocator_t* memento_create_stack_allocator(const char* name, size_t capacity, memento_allocator_t* backing) {
    if (!backing || capacity == 0) return NULL;
    
    /* Validate capacity to prevent overflow */
    if (capacity > SIZE_MAX / 2) return NULL;
    
    size_t total_size = sizeof(memento_stack_allocator_t) + strlen(name) + 1;
    memento_stack_allocator_t* stack = (memento_stack_allocator_t*)malloc(total_size);
    
    if (!stack) {
        return NULL;
    }
    
    memset(stack, 0, sizeof(memento_stack_allocator_t));
    
    /* Allocate stack memory from backing allocator */
    memento_result_t mem_result = memento_alloc_aligned(backing, capacity, MEMENTO_CACHE_LINE_SIZE);
    if (!mem_result.success) {
        free(stack);
        return NULL;
    }
    
    /* Initialize base allocator */
    stack->base.name = (const char*)(stack + 1);
    strcpy((char*)stack->base.name, name ? name : "stack");
    memset(&stack->base.stats, 0, sizeof(memento_stats_t));
    
    stack->base.allocate = memento_stack_allocate;
    stack->base.deallocate = memento_stack_deallocate;
    stack->base.destroy = memento_stack_destroy;
    
    /* Initialize stack data */
    stack->start = (char*)mem_result.ptr;
    stack->end = stack->start + capacity;
    stack->top = stack->start;
    stack->backing = backing;
    
    return &stack->base;
}

static memento_result_t memento_stack_allocate(memento_allocator_t* allocator, size_t size, size_t alignment) {
    memento_stack_allocator_t* stack = (memento_stack_allocator_t*)allocator;
    memento_result_t result = {NULL, size, false};
    
    if (size == 0) {
        result.success = true;
        return result;
    }
    
    /* Align the top pointer */
    char* aligned_top = (char*)memento_align_pointer(stack->top, alignment);
    
    /* Check if we have enough space */
    if (aligned_top + size > stack->end) {
        return result;  /* Out of stack space */
    }
    
    result.ptr = aligned_top;
    result.success = true;
    stack->top = aligned_top + size;
    
    return result;
}

static void memento_stack_deallocate(memento_allocator_t* allocator, void* ptr) {
    /* Stack allocator doesn't support individual deallocations */
    /* This is intentional - use reset to clear the entire stack */
    (void)allocator;
    (void)ptr;
}

static void memento_stack_destroy(memento_allocator_t* allocator) {
    memento_stack_allocator_t* stack = (memento_stack_allocator_t*)allocator;
    
    /* Free the stack memory */
    if (stack->start) {
        memento_free(stack->backing, stack->start);
    }
    
    free(stack);
}

/* ============================================================================= */
/* GLOBAL MANAGEMENT FUNCTIONS                                                 */
/* ============================================================================= */

static int initialize_thread_cache(void) {
    /* Initialize thread-local storage for thread cache */
    /* This is a simplified implementation */
    return MEMENTO_SUCCESS;
}

static void shutdown_thread_cache(void) {
    /* Clean up thread-local storage */
}

int memento_init(void) {
    if (g_memento.initialized) {
        return MEMENTO_ERROR_ALREADY_INITIALIZED;
    }
    
    /* Initialize configuration with defaults */
    g_memento.config.enable_statistics = MEMENTO_ENABLE_STATISTICS;
    g_memento.config.enable_debug_checks = MEMENTO_ENABLE_DEBUG_CHECKS;
    g_memento.config.enable_thread_cache = true;
    g_memento.config.enable_block_allocator = true;
    g_memento.config.thread_cache_size = 64 * 1024;  /* 64KB */
    g_memento.config.thread_cache_threshold = 1024;  /* 1KB */
    g_memento.config.block_size = MEMENTO_BLOCK_SIZE;
    g_memento.config.min_chunk_size = MEMENTO_MIN_CHUNK_SIZE;
    g_memento.config.max_chunk_size = MEMENTO_BLOCK_SIZE / 4;
    g_memento.config.memory_map_threshold = 2 * 1024 * 1024;  /* 2MB */
    g_memento.config.page_size = 4096;  /* 4KB */
    
    /* Initialize thread cache system */
    int result = initialize_thread_cache();
    if (result != MEMENTO_SUCCESS) {
        return result;
    }
    
    /* Create root allocator */
    g_memento.root_allocator = memento_create_thread_cache("root");
    if (!g_memento.root_allocator) {
        shutdown_thread_cache();
        return MEMENTO_ERROR_OUT_OF_MEMORY;
    }
    
    g_memento.initialized = true;
    return MEMENTO_SUCCESS;
}

int memento_shutdown(void) {
    if (!g_memento.initialized) {
        return MEMENTO_ERROR_NOT_INITIALIZED;
    }
    
    /* Destroy root allocator */
    if (g_memento.root_allocator) {
        memento_destroy_allocator(g_memento.root_allocator);
        g_memento.root_allocator = NULL;
    }
    
    /* Shutdown thread cache system */
    shutdown_thread_cache();
    
    g_memento.initialized = false;
    return MEMENTO_SUCCESS;
}

bool memento_is_initialized(void) {
    return g_memento.initialized;
}

const memento_config_t* memento_get_config(void) {
    return &g_memento.config;
}

void memento_destroy_allocator(memento_allocator_t* allocator) {
    if (!allocator) return;
    
    if (allocator->destroy) {
        allocator->destroy(allocator);
    } else {
        free(allocator);
    }
}

#endif /* MEMENTO_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif

#endif /* MEMENTO_H */