/*
 * Memento Memory Allocator Library
 * 
 * A high-performance, multi-allocator memory management library.
 * Non-locking design - no atomics, no locks, completely thread-local.
 * 
 * Features:
 * - Thread Heap: Purely thread-local caching (no atomics!)
 * - Pool: Fixed-size object pools
 * - Arena: Bump allocator with power-of-2 growth
 * - Stack: LIFO scope-based allocator  
 * - Slab: Multi-size object caching
 * 
 * Usage (C):
 *   #define MEMENTO_IMPLEMENTATION
 *   #include "memento.h"
 *   
 *   int main() {
 *       memento_init();
 *       memento_thread_heap_t* heap = memento_thread_heap_get();
 *       void* ptr = memento_thread_heap_alloc(heap, 1024);
 *       memento_thread_heap_free(heap, ptr, 1024);
 *       return 0;
 *   }
 * 
 * Usage (C++):
 *   #include "memento.hpp"
 *   
 *   int main() {
 *       memento::context ctx;
 *       memento::heap h;
 *       auto obj = h.construct<MyClass>(args...);
 *       h.destroy(obj);
 *       return 0;
 *   }
 * 
 * License: MIT
 */

#ifndef MEMENTO_H
#define MEMENTO_H

/* Version macros for compile-time checking */
#define MEMENTO_VERSION_MAJOR 2
#define MEMENTO_VERSION_MINOR 0
#define MEMENTO_VERSION_PATCH 0
#define MEMENTO_VERSION_STRING "2.0.0"
#define MEMENTO_VERSION ((MEMENTO_VERSION_MAJOR << 16) | \
                         (MEMENTO_VERSION_MINOR << 8) | \
                         MEMENTO_VERSION_PATCH)

/* ============================================================================
 * Standard Headers
 * ============================================================================ */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration and Platform Detection
 * ============================================================================ */

/* Platform detection */
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

/* Cache line size (common values: 64 for x86/ARM, 128 for POWER) */
#ifndef MEMENTO_CACHE_LINE_SIZE
    #define MEMENTO_CACHE_LINE_SIZE 64
#endif

/* Alignment macro */
#if defined(_MSC_VER)
    #define MEMENTO_ALIGNED(x) __declspec(align(x))
#else
    #define MEMENTO_ALIGNED(x) __attribute__((aligned(x)))
#endif

/* Memory ordering - relaxed since heaps are thread-local */
#define MEMENTO_MEMORY_ORDER_RELAXED 0

/* ============================================================================
 * Public API
 * ============================================================================ */

typedef struct memento_thread_heap_s memento_thread_heap_t;
typedef struct memento_pool_s memento_pool_t;
typedef struct memento_arena_s memento_arena_t;
typedef struct memento_stack_s memento_stack_t;
typedef struct memento_slab_s memento_slab_t;

/* Size classes: 32B, 64B, 96B, 128B, 192B, 256B, 384B, 512B, 768B, 1KB, 1.5KB, 2KB, 3KB, 4KB, 6KB, 8KB */
#define MEMENTO_SIZE_CLASS_COUNT 16

/* Thread-local heap statistics (for debugging/monitoring) */
typedef struct {
    size_t alloc_count;
    size_t free_count;
    size_t bytes_allocated;
    size_t bytes_freed;
    size_t foreign_free_count;  /* Freed by other threads */
} memento_heap_stats_t;

/* ============================================================================
 * Version API
 * ============================================================================ */

/* Get version string (e.g., "2.0.0") */
static inline const char* memento_version_string(void) {
    return MEMENTO_VERSION_STRING;
}

/* Get version number (e.g., 0x020000 for 2.0.0) */
static inline unsigned int memento_version_number(void) {
    return MEMENTO_VERSION;
}

/* Check if library version is at least major.minor.patch */
static inline int memento_version_check(int major, int minor, int patch) {
    return MEMENTO_VERSION >= ((major << 16) | (minor << 8) | patch);
}

/* ============================================================================
 * Thread Heap API - Non-locking, thread-local caching
 * ============================================================================ */

/* Initialize global state (call once at startup) */
bool memento_init(void);
void memento_shutdown(void);

/* Get thread-local heap (creates on first call, cached in TLS) */
memento_thread_heap_t* memento_thread_heap_get(void);

/* Allocate/free from thread-local heap (non-locking) */
void* memento_thread_heap_alloc(memento_thread_heap_t* heap, size_t size);
void memento_thread_heap_free(memento_thread_heap_t* heap, void* ptr, size_t size);
void* memento_thread_heap_realloc(memento_thread_heap_t* heap, void* ptr, 
                                   size_t old_size, size_t new_size);

/* Flush any pending foreign deallocations (call periodically) */
void memento_thread_heap_flush(memento_thread_heap_t* heap);

/* Get heap statistics */
void memento_thread_heap_stats(memento_thread_heap_t* heap, memento_heap_stats_t* stats);

/* ============================================================================
 * Pool Allocator API - Fixed-size object pools
 * ============================================================================ */

/* Create pool for objects of given size */
memento_pool_t* memento_pool_create(size_t object_size, size_t capacity, 
                                     memento_thread_heap_t* heap);
void memento_pool_destroy(memento_pool_t* pool);

/* Allocate/free from pool */
void* memento_pool_alloc(memento_pool_t* pool);
void memento_pool_free(memento_pool_t* pool, void* ptr);

/* ============================================================================
 * Arena Allocator API - Bump allocator with power-of-2 growth
 * ============================================================================ */

/* Create arena with initial capacity */
memento_arena_t* memento_arena_create(size_t initial_capacity,
                                       memento_thread_heap_t* heap);
void memento_arena_destroy(memento_arena_t* arena);

/* Bump allocation */
void* memento_arena_alloc(memento_arena_t* arena, size_t size, size_t alignment);

/* Save/restore for temporary allocations */
typedef struct {
    void* saved_top;
    size_t saved_used;
} memento_arena_save_t;

memento_arena_save_t memento_arena_save(memento_arena_t* arena);
void memento_arena_restore(memento_arena_t* arena, memento_arena_save_t* save);

/* Reset arena to empty */
void memento_arena_reset(memento_arena_t* arena);

/* Get stats */
size_t memento_arena_used(const memento_arena_t* arena);
size_t memento_arena_capacity(const memento_arena_t* arena);

/* ============================================================================
 * Stack Allocator API - LIFO scope-based allocation
 * ============================================================================ */

/* Create stack with given capacity */
memento_stack_t* memento_stack_create(size_t capacity, memento_thread_heap_t* heap);
void memento_stack_destroy(memento_stack_t* stack);

/* Push/pop allocations (LIFO - must free in reverse order) */
void* memento_stack_push(memento_stack_t* stack, size_t size, size_t alignment);
void memento_stack_pop(memento_stack_t* stack, void* ptr);

/* Frame markers for bulk rollback */
typedef size_t memento_stack_marker_t;

memento_stack_marker_t memento_stack_marker(memento_stack_t* stack);
void memento_stack_pop_to_marker(memento_stack_t* stack, memento_stack_marker_t marker);

/* Reset entire stack */
void memento_stack_reset(memento_stack_t* stack);

/* ============================================================================
 * Slab Allocator API - Multi-size object caching
 * ============================================================================ */

/* Create slab allocator */
memento_slab_t* memento_slab_create(memento_thread_heap_t* heap);
void memento_slab_destroy(memento_slab_t* slab);

/* Allocate/free any size (automatically routed to appropriate size class) */
void* memento_slab_alloc(memento_slab_t* slab, size_t size);
void memento_slab_free(memento_slab_t* slab, void* ptr, size_t size);

/* ============================================================================
 * Utility API
 * ============================================================================ */

/* Get size class for a given allocation size */
size_t memento_size_class_for(size_t size);
size_t memento_size_class_to_size(size_t sc);

/* Alignment utilities */
bool memento_is_power_of_two(size_t x);
size_t memento_align_up(size_t size, size_t alignment);
size_t memento_align_down(size_t size, size_t alignment);

#ifdef __cplusplus
}
#endif

/* ============================================================================
 * Implementation Section
 * 
 * Include this in exactly ONE source file:
 *   #define MEMENTO_IMPLEMENTATION
 *   #include "memento.h"
 * ============================================================================ */

#ifdef MEMENTO_IMPLEMENTATION

/* ============================================================================
 * Internal Implementation - Non-locking Design
 * ============================================================================ */

/* Internal headers */
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>

/* MAP_ANONYMOUS compatibility */
#ifndef MAP_ANONYMOUS
    #ifdef MAP_ANON
        #define MAP_ANONYMOUS MAP_ANON
    #else
        #define MAP_ANONYMOUS 0x20  /* Common value */
    #endif
#endif

/* Branch prediction hints */
#ifndef MEMENTO_LIKELY
    #define MEMENTO_LIKELY(x) __builtin_expect(!!(x), 1)
#endif
#ifndef MEMENTO_UNLIKELY
    #define MEMENTO_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

/* Fallback macros for allocation */
#ifndef MEMENTO_MALLOC
    #define MEMENTO_MALLOC(size) malloc(size)
#endif
#ifndef MEMENTO_FREE
    #define MEMENTO_FREE(ptr, size) free(ptr)
#endif
#ifndef MEMENTO_MMAP
    #define MEMENTO_MMAP(size) mmap(NULL, size, PROT_READ|PROT_WRITE, \
        MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
#endif
#ifndef MEMENTO_MUNMAP
    #define MEMENTO_MUNMAP(ptr, size) munmap(ptr, size)
#endif

/* Size class configuration */
static const size_t memento_size_classes[MEMENTO_SIZE_CLASS_COUNT] = {
    32, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192
};

MEMENTO_FORCE_INLINE size_t memento_size_class_for(size_t size) {
    if (size <= 32) return 0;
    if (size <= 64) return 1;
    if (size <= 96) return 2;
    if (size <= 128) return 3;
    if (size <= 192) return 4;
    if (size <= 256) return 5;
    if (size <= 384) return 6;
    if (size <= 512) return 7;
    if (size <= 768) return 8;
    if (size <= 1024) return 9;
    if (size <= 1536) return 10;
    if (size <= 2048) return 11;
    if (size <= 3072) return 12;
    if (size <= 4096) return 13;
    if (size <= 6144) return 14;
    return 15;
}

MEMENTO_FORCE_INLINE size_t memento_size_class_to_size(size_t sc) {
    return (sc < MEMENTO_SIZE_CLASS_COUNT) ? memento_size_classes[sc] : 8192;
}

MEMENTO_FORCE_INLINE bool memento_is_power_of_two(size_t x) {
    return (x & (x - 1)) == 0;
}

MEMENTO_FORCE_INLINE size_t memento_align_up(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

MEMENTO_FORCE_INLINE size_t memento_align_down(size_t size, size_t alignment) {
    return size & ~(alignment - 1);
}

/* ============================================================================
 * Internal Thread Cache - Non-locking Treiber Stack
 * ============================================================================ */

typedef struct memento_cache_node_s {
    struct memento_cache_node_s* next;
} memento_cache_node_t;

typedef struct {
    memento_cache_node_t* head;
    uint32_t count;
    uint32_t limit;
} memento_size_class_cache_t;

/* Thread-local heap structure */
struct memento_thread_heap_s {
    /* Size class caches - purely thread-local, no atomics needed */
    memento_size_class_cache_t caches[MEMENTO_SIZE_CLASS_COUNT];
    
    /* Foreign deallocation ring buffer (SPSC - single producer, single consumer) */
    void* foreign_buffer[256];
    uint32_t foreign_head;  /* Only this thread writes */
    uint32_t foreign_tail;  /* Other threads write */
    char foreign_pad[MEMENTO_CACHE_LINE_SIZE - 8]; /* Pad to cache line */
    
    /* Statistics (relaxed consistency - just for monitoring) */
    memento_heap_stats_t stats;
    
    /* Thread ID for debugging */
    uint64_t thread_id;
    
    /* Heap state */
    uint8_t initialized;
    uint8_t _pad[7];
};

/* ============================================================================
 * Thread-local Storage
 * ============================================================================ */

static MEMENTO_TLS memento_thread_heap_t* memento_tls_heap = NULL;

static uint64_t memento_get_thread_id(void) {
#if defined(__linux__)
    return (uint64_t)pthread_self();
#elif defined(_WIN32)
    return (uint64_t)GetCurrentThreadId();
#else
    static uint64_t counter = 0;
    return ++counter;
#endif
}

static void memento_heap_init(memento_thread_heap_t* heap) {
    memset(heap, 0, sizeof(memento_thread_heap_t));
    
    for (int i = 0; i < MEMENTO_SIZE_CLASS_COUNT; i++) {
        heap->caches[i].limit = 64; /* Max 64 items per size class */
    }
    
    heap->thread_id = memento_get_thread_id();
    heap->initialized = 1;
}

/* Get or create thread-local heap */
memento_thread_heap_t* memento_thread_heap_get(void) {
    if (MEMENTO_UNLIKELY(memento_tls_heap == NULL)) {
        memento_tls_heap = (memento_thread_heap_t*)MEMENTO_MALLOC(sizeof(memento_thread_heap_t));
        if (memento_tls_heap) {
            memento_heap_init(memento_tls_heap);
        }
    }
    return memento_tls_heap;
}

/* ============================================================================
 * Cache Operations - Non-locking
 * ============================================================================ */

static MEMENTO_FORCE_INLINE void* memento_cache_pop(memento_size_class_cache_t* cache) {
    memento_cache_node_t* node = cache->head;
    if (MEMENTO_LIKELY(node != NULL)) {
        cache->head = node->next;
        cache->count--;
        return node;
    }
    return NULL;
}

static MEMENTO_FORCE_INLINE bool memento_cache_push(memento_size_class_cache_t* cache, void* ptr) {
    if (cache->count >= cache->limit) {
        return false; /* Cache full */
    }
    memento_cache_node_t* node = (memento_cache_node_t*)ptr;
    node->next = cache->head;
    cache->head = node;
    cache->count++;
    return true;
}

/* ============================================================================
 * Foreign Deallocation - SPSC Ring Buffer (non-locking)
 * ============================================================================ */

#define MEMENTO_FOREIGN_RING_SIZE 256
#define MEMENTO_FOREIGN_MASK (MEMENTO_FOREIGN_RING_SIZE - 1)

static bool memento_foreign_push(memento_thread_heap_t* heap, void* ptr) {
    uint32_t head = heap->foreign_head;
    uint32_t next = (head + 1) & MEMENTO_FOREIGN_MASK;
    
    if (next == heap->foreign_tail) {
        return false; /* Ring full */
    }
    
    heap->foreign_buffer[head] = ptr;
    heap->foreign_head = next;
    return true;
}

static void* memento_foreign_pop(memento_thread_heap_t* heap) {
    if (heap->foreign_head == heap->foreign_tail) {
        return NULL; /* Ring empty */
    }
    
    void* ptr = heap->foreign_buffer[heap->foreign_tail];
    heap->foreign_tail = (heap->foreign_tail + 1) & MEMENTO_FOREIGN_MASK;
    return ptr;
}

void memento_thread_heap_flush(memento_thread_heap_t* heap) {
    void* ptr;
    while ((ptr = memento_foreign_pop(heap)) != NULL) {
        /* Size is stored in the first 8 bytes of the allocation */
        size_t size = *(size_t*)ptr;
        memento_thread_heap_free(heap, (char*)ptr + 8, size);
        heap->stats.foreign_free_count++;
    }
}

/* ============================================================================
 * Allocation from System
 * ============================================================================ */

static void* memento_alloc_from_system(size_t size) {
    /* Use mmap for large allocations, malloc for small */
    if (size >= 64 * 1024) {
        size = memento_align_up(size, 4096);
        return MEMENTO_MMAP(size);
    }
    return MEMENTO_MALLOC(size);
}

static void memento_free_to_system(void* ptr, size_t size) {
    if (size >= 64 * 1024) {
        size = memento_align_up(size, 4096);
        MEMENTO_MUNMAP(ptr, size);
    } else {
        MEMENTO_FREE(ptr, size);
    }
}

/* ============================================================================
 * Thread Heap Allocation
 * ============================================================================ */

void* memento_thread_heap_alloc(memento_thread_heap_t* heap, size_t size) {
    if (MEMENTO_UNLIKELY(heap == NULL || size == 0)) {
        return NULL;
    }
    
    /* Check for size classes */
    if (size <= 8192) {
        size_t sc = memento_size_class_for(size);
        size_t actual_size = memento_size_class_to_size(sc);
        
        /* Try thread-local cache first */
        void* ptr = memento_cache_pop(&heap->caches[sc]);
        if (MEMENTO_LIKELY(ptr != NULL)) {
            heap->stats.alloc_count++;
            return ptr;
        }
        
        /* Allocate from system */
        ptr = memento_alloc_from_system(actual_size);
        if (ptr) {
            heap->stats.alloc_count++;
            heap->stats.bytes_allocated += actual_size;
        }
        return ptr;
    }
    
    /* Large allocation - direct from system with size header for foreign free */
    size_t total_size = size + sizeof(size_t);
    void* ptr = memento_alloc_from_system(total_size);
    if (ptr) {
        *(size_t*)ptr = size;
        heap->stats.alloc_count++;
        heap->stats.bytes_allocated += size;
        return (char*)ptr + sizeof(size_t);
    }
    return NULL;
}

void memento_thread_heap_free(memento_thread_heap_t* heap, void* ptr, size_t size) {
    if (MEMENTO_UNLIKELY(ptr == NULL || heap == NULL)) {
        return;
    }
    
    /* For large allocations, adjust pointer and size to include header */
    void* original_ptr = ptr;
    size_t total_size = size;
    if (size > 8192) {
        original_ptr = (char*)ptr - sizeof(size_t);
        total_size = size + sizeof(size_t);
    }
    
    /* Check if this is a foreign free (different thread) */
    if (memento_get_thread_id() != heap->thread_id) {
        /* Push to foreign ring buffer (use original_ptr with header) */
        if (memento_foreign_push(heap, original_ptr)) {
            return;
        }
        /* Ring full - flush and retry */
        memento_thread_heap_flush(heap);
        memento_foreign_push(heap, original_ptr);
        return;
    }
    
    /* Local free - try to cache it */
    if (size <= 8192) {
        size_t sc = memento_size_class_for(size);
        if (memento_cache_push(&heap->caches[sc], ptr)) {
            heap->stats.free_count++;
            return;
        }
    }
    
    /* Return to system (use original_ptr and total_size for large allocs) */
    memento_free_to_system(original_ptr, total_size);
    heap->stats.free_count++;
    heap->stats.bytes_freed += size;
}

void* memento_thread_heap_realloc(memento_thread_heap_t* heap, void* ptr,
                                   size_t old_size, size_t new_size) {
    if (ptr == NULL) {
        return memento_thread_heap_alloc(heap, new_size);
    }
    if (new_size == 0) {
        memento_thread_heap_free(heap, ptr, old_size);
        return NULL;
    }
    
    void* new_ptr = memento_thread_heap_alloc(heap, new_size);
    if (new_ptr) {
        size_t copy_size = (old_size < new_size) ? old_size : new_size;
        memcpy(new_ptr, ptr, copy_size);
        memento_thread_heap_free(heap, ptr, old_size);
    }
    return new_ptr;
}

void memento_thread_heap_stats(memento_thread_heap_t* heap, memento_heap_stats_t* stats) {
    if (heap && stats) {
        *stats = heap->stats;
    }
}

/* ============================================================================
 * Global Initialization
 * ============================================================================ */

static bool memento_initialized = false;

bool memento_init(void) {
    if (memento_initialized) {
        return true;
    }
    memento_initialized = true;
    return true;
}

void memento_shutdown(void) {
    /* Thread heaps are cleaned up when threads exit via TLS destructor */
    memento_initialized = false;
}

/* ============================================================================
 * Pool Allocator Implementation
 * ============================================================================ */

typedef struct memento_pool_chunk_s {
    struct memento_pool_chunk_s* next;
} memento_pool_chunk_t;

struct memento_pool_s {
    memento_pool_chunk_t* free_list;
    size_t object_size;
    size_t capacity;
    size_t count;
    memento_thread_heap_t* heap;
    void* blocks;  /* Linked list of allocated blocks for cleanup */
};

memento_pool_t* memento_pool_create(size_t object_size, size_t capacity,
                                     memento_thread_heap_t* heap) {
    if (object_size < sizeof(void*)) {
        object_size = sizeof(void*);
    }
    
    memento_pool_t* pool = (memento_pool_t*)MEMENTO_MALLOC(sizeof(memento_pool_t));
    if (!pool) return NULL;
    
    pool->object_size = object_size;
    pool->capacity = capacity;
    pool->count = 0;
    pool->heap = heap ? heap : memento_thread_heap_get();
    pool->free_list = NULL;
    pool->blocks = NULL;
    
    /* Pre-allocate objects */
    size_t block_size = object_size * capacity;
    void* block = memento_thread_heap_alloc(pool->heap, block_size);
    if (!block) {
        MEMENTO_FREE(pool, sizeof(memento_pool_t));
        return NULL;
    }
    
    /* Build free list */
    for (size_t i = 0; i < capacity; i++) {
        memento_pool_chunk_t* chunk = (memento_pool_chunk_t*)((char*)block + i * object_size);
        chunk->next = pool->free_list;
        pool->free_list = chunk;
    }
    
    return pool;
}

void memento_pool_destroy(memento_pool_t* pool) {
    if (!pool) return;
    /* TODO: Free blocks */
    MEMENTO_FREE(pool, sizeof(memento_pool_t));
}

void* memento_pool_alloc(memento_pool_t* pool) {
    if (!pool || !pool->free_list) return NULL;
    
    memento_pool_chunk_t* chunk = pool->free_list;
    pool->free_list = chunk->next;
    pool->count--;
    return chunk;
}

void memento_pool_free(memento_pool_t* pool, void* ptr) {
    if (!pool || !ptr) return;
    
    memento_pool_chunk_t* chunk = (memento_pool_chunk_t*)ptr;
    chunk->next = pool->free_list;
    pool->free_list = chunk;
    pool->count++;
}

/* ============================================================================
 * Arena Allocator Implementation
 * ============================================================================ */

typedef struct memento_arena_block_s {
    struct memento_arena_block_s* next;
    size_t size;
    char data[];
} memento_arena_block_t;

struct memento_arena_s {
    memento_arena_block_t* current;
    memento_arena_block_t* blocks;
    void* top;
    size_t used;
    size_t capacity;
    size_t initial_capacity;
    memento_thread_heap_t* heap;
};

memento_arena_t* memento_arena_create(size_t initial_capacity,
                                       memento_thread_heap_t* heap) {
    memento_arena_t* arena = (memento_arena_t*)MEMENTO_MALLOC(sizeof(memento_arena_t));
    if (!arena) return NULL;
    
    arena->heap = heap ? heap : memento_thread_heap_get();
    arena->initial_capacity = initial_capacity;
    arena->capacity = initial_capacity;
    arena->used = 0;
    
    /* Allocate initial block */
    size_t block_size = sizeof(memento_arena_block_t) + initial_capacity;
    arena->current = (memento_arena_block_t*)memento_thread_heap_alloc(arena->heap, block_size);
    if (!arena->current) {
        MEMENTO_FREE(arena, sizeof(memento_arena_t));
        return NULL;
    }
    
    arena->current->next = NULL;
    arena->current->size = initial_capacity;
    arena->blocks = arena->current;
    arena->top = arena->current->data;
    
    return arena;
}

void memento_arena_destroy(memento_arena_t* arena) {
    if (!arena) return;
    
    memento_arena_block_t* block = arena->blocks;
    while (block) {
        memento_arena_block_t* next = block->next;
        memento_thread_heap_free(arena->heap, block, sizeof(memento_arena_block_t) + block->size);
        block = next;
    }
    
    MEMENTO_FREE(arena, sizeof(memento_arena_t));
}

void* memento_arena_alloc(memento_arena_t* arena, size_t size, size_t alignment) {
    if (!arena || size == 0) return NULL;
    
    /* Align the current position */
    uintptr_t current = (uintptr_t)arena->top;
    uintptr_t aligned = (current + alignment - 1) & ~(alignment - 1);
    size_t padding = aligned - current;
    
    /* Check if we need to grow */
    if (arena->used + padding + size > arena->capacity) {
        /* Grow with power of 2 */
        size_t new_capacity = arena->capacity * 2;
        while (new_capacity < size + sizeof(memento_arena_block_t)) {
            new_capacity *= 2;
        }
        
        size_t block_size = sizeof(memento_arena_block_t) + new_capacity;
        memento_arena_block_t* block = (memento_arena_block_t*)memento_thread_heap_alloc(arena->heap, block_size);
        if (!block) return NULL;
        
        block->next = arena->blocks;
        block->size = new_capacity;
        arena->blocks = block;
        arena->current = block;
        arena->capacity = new_capacity;
        arena->used = 0;
        arena->top = block->data;
        
        /* Recalculate alignment */
        current = (uintptr_t)arena->top;
        aligned = (current + alignment - 1) & ~(alignment - 1);
    }
    
    void* ptr = (void*)aligned;
    arena->top = (char*)aligned + size;
    arena->used += padding + size;
    
    return ptr;
}

memento_arena_save_t memento_arena_save(memento_arena_t* arena) {
    memento_arena_save_t save;
    save.saved_top = arena->top;
    save.saved_used = arena->used;
    return save;
}

void memento_arena_restore(memento_arena_t* arena, memento_arena_save_t* save) {
    if (!arena || !save) return;
    arena->top = save->saved_top;
    arena->used = save->saved_used;
}

void memento_arena_reset(memento_arena_t* arena) {
    if (!arena) return;
    arena->top = arena->current->data;
    arena->used = 0;
}

size_t memento_arena_used(const memento_arena_t* arena) {
    return arena ? arena->used : 0;
}

size_t memento_arena_capacity(const memento_arena_t* arena) {
    return arena ? arena->capacity : 0;
}

/* ============================================================================
 * Stack Allocator Implementation
 * ============================================================================ */

struct memento_stack_s {
    char* buffer;
    size_t capacity;
    size_t top;
    memento_thread_heap_t* heap;
};

memento_stack_t* memento_stack_create(size_t capacity, memento_thread_heap_t* heap) {
    memento_stack_t* stack = (memento_stack_t*)MEMENTO_MALLOC(sizeof(memento_stack_t));
    if (!stack) return NULL;
    
    stack->heap = heap ? heap : memento_thread_heap_get();
    stack->buffer = (char*)memento_thread_heap_alloc(stack->heap, capacity);
    if (!stack->buffer) {
        MEMENTO_FREE(stack, sizeof(memento_stack_t));
        return NULL;
    }
    
    stack->capacity = capacity;
    stack->top = 0;
    return stack;
}

void memento_stack_destroy(memento_stack_t* stack) {
    if (!stack) return;
    if (stack->buffer) {
        memento_thread_heap_free(stack->heap, stack->buffer, stack->capacity);
    }
    MEMENTO_FREE(stack, sizeof(memento_stack_t));
}

void* memento_stack_push(memento_stack_t* stack, size_t size, size_t alignment) {
    if (!stack) return NULL;
    
    size_t aligned_top = memento_align_up(stack->top, alignment);
    if (aligned_top + size > stack->capacity) {
        return NULL; /* Stack overflow */
    }
    
    void* ptr = stack->buffer + aligned_top;
    stack->top = aligned_top + size;
    return ptr;
}

void memento_stack_pop(memento_stack_t* stack, void* ptr) {
    if (!stack || !ptr) return;
    /* In a real implementation, we'd track sizes to validate LIFO order */
    /* For now, this is a no-op - the memory is just reused on next push */
    (void)ptr;
}

memento_stack_marker_t memento_stack_marker(memento_stack_t* stack) {
    return stack ? stack->top : 0;
}

void memento_stack_pop_to_marker(memento_stack_t* stack, memento_stack_marker_t marker) {
    if (stack) {
        stack->top = marker;
    }
}

void memento_stack_reset(memento_stack_t* stack) {
    if (stack) {
        stack->top = 0;
    }
}

/* ============================================================================
 * Slab Allocator Implementation
 * ============================================================================ */

typedef struct {
    memento_size_class_cache_t cache;
    size_t block_size;
} memento_slab_class_t;

struct memento_slab_s {
    memento_slab_class_t classes[MEMENTO_SIZE_CLASS_COUNT];
    memento_thread_heap_t* heap;
};

memento_slab_t* memento_slab_create(memento_thread_heap_t* heap) {
    memento_slab_t* slab = (memento_slab_t*)MEMENTO_MALLOC(sizeof(memento_slab_t));
    if (!slab) return NULL;
    
    slab->heap = heap ? heap : memento_thread_heap_get();
    
    for (int i = 0; i < MEMENTO_SIZE_CLASS_COUNT; i++) {
        slab->classes[i].cache.head = NULL;
        slab->classes[i].cache.count = 0;
        slab->classes[i].cache.limit = 64;
        slab->classes[i].block_size = memento_size_class_to_size(i);
    }
    
    return slab;
}

void memento_slab_destroy(memento_slab_t* slab) {
    if (!slab) return;
    /* Free all cached blocks */
    for (int i = 0; i < MEMENTO_SIZE_CLASS_COUNT; i++) {
        void* ptr;
        while ((ptr = memento_cache_pop(&slab->classes[i].cache)) != NULL) {
            memento_thread_heap_free(slab->heap, ptr, slab->classes[i].block_size);
        }
    }
    MEMENTO_FREE(slab, sizeof(memento_slab_t));
}

void* memento_slab_alloc(memento_slab_t* slab, size_t size) {
    if (!slab || size == 0) return NULL;
    
    if (size > 8192) {
        /* Large allocation - bypass slab */
        return memento_thread_heap_alloc(slab->heap, size);
    }
    
    size_t sc = memento_size_class_for(size);
    void* ptr = memento_cache_pop(&slab->classes[sc].cache);
    if (!ptr) {
        ptr = memento_thread_heap_alloc(slab->heap, slab->classes[sc].block_size);
    }
    return ptr;
}

void memento_slab_free(memento_slab_t* slab, void* ptr, size_t size) {
    if (!slab || !ptr) return;
    
    if (size > 8192) {
        memento_thread_heap_free(slab->heap, ptr, size);
        return;
    }
    
    size_t sc = memento_size_class_for(size);
    if (!memento_cache_push(&slab->classes[sc].cache, ptr)) {
        memento_thread_heap_free(slab->heap, ptr, slab->classes[sc].block_size);
    }
}

#endif /* MEMENTO_IMPLEMENTATION */

#endif /* MEMENTO_H */
