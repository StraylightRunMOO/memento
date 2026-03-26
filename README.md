# Memento - High-Performance Memory Allocator

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Standard](https://img.shields.io/badge/C-99-blue.svg)](https://en.wikipedia.org/wiki/C99)
[![Standard](https://img.shields.io/badge/C++-17-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B17)

**Memento** is a high-performance, multi-strategy memory allocator library (version 2.0.0) designed for modern multi-threaded applications. It combines the best ideas from production allocators like **rpmalloc** and **mimalloc** with a focus on simplicity, performance, and flexibility.

## Key Features

- **Non-Locking Design** - Zero atomics on the hot path, pure thread-local caching
- **Single Header** - Drop `memento.h` into your project and go
- **Multiple Allocators** - Choose the right strategy for your use case
- **Thread-Safe** - Each thread owns its heap, no contention
- **C++17/20 Support** - Modern C++ wrapper with RAII and STL integration
- **Benchmarked** - Competitive with rpmalloc and mimalloc

## Quick Start

### C (C99)

```c
#define MEMENTO_IMPLEMENTATION
#include "memento.h"

int main() {
    memento_init();
    
    // Get thread-local heap
    memento_thread_heap_t* heap = memento_thread_heap_get();
    
    // Allocate and free
    void* ptr = memento_thread_heap_alloc(heap, 1024);
    // ... use ptr ...
    memento_thread_heap_free(heap, ptr, 1024);
    
    memento_shutdown();
    return 0;
}
```

### C++ (C++17)

```cpp
#define MEMENTO_IMPLEMENTATION
#include "memento.hpp"

int main() {
    memento::context ctx;  // RAII initialization
    memento::heap h;       // Thread-local heap
    
    // Object construction with automatic destruction
    auto obj = h.construct<MyClass>(constructor_args...);
    h.destroy(obj);
    
    // STL containers
    std::vector<int, memento::allocator<int>> vec;
    vec.push_back(42);
    
    return 0;
}
```

## Allocator Types

### 1. Thread Heap - General Purpose
The default allocator with lock-free thread-local caching.

```c
memento_thread_heap_t* heap = memento_thread_heap_get();
void* ptr = memento_thread_heap_alloc(heap, size);
memento_thread_heap_free(heap, ptr, size);
```

**Best for:** Most allocations, high-frequency operations

### 2. Pool - Fixed-Size Objects
O(1) allocation/deallocation for objects of the same size.

```c
memento_pool_t* pool = memento_pool_create(sizeof(MyStruct), 1000, heap);
void* obj = memento_pool_alloc(pool);
memento_pool_free(pool, obj);
```

**Best for:** Game entities, network packets, job structs

### 3. Arena - Temporary Allocations
Bump pointer allocator with save/restore points.

```c
memento_arena_t* arena = memento_arena_create(64*1024, heap);
void* tmp = memento_arena_alloc(arena, size, alignment);
memento_arena_reset(arena);  // Free all at once
```

**Best for:** Frame allocations, parsing, compilation

### 4. Stack - LIFO Patterns
Scope-based allocation with frame markers.

```c
memento_stack_t* stack = memento_stack_create(4096, heap);
void* ptr = memento_stack_push(stack, size, alignment);
memento_stack_marker_t mark = memento_stack_marker(stack);
// ... more pushes ...
memento_stack_pop_to_marker(stack, mark);  // Bulk rollback
```

**Best for:** Recursive algorithms, expression evaluation

### 5. Slab - Multi-Size Caching
Automatic size-class routing with per-size caches.

```c
memento_slab_t* slab = memento_slab_create(heap);
void* ptr = memento_slab_alloc(slab, size);
memento_slab_free(slab, ptr, size);
```

**Best for:** Variable-size allocations with caching

## Performance

Memento uses a **non-locking** design inspired by rpmalloc:

- **Thread-local heaps** - Each thread owns its memory
- **No atomics on hot path** - Pure thread-local operations
- **16 size classes** - 32B to 8KB with power-of-2 spacing
- **SPSC foreign-free ring** - Cross-thread deallocation without locks

### Benchmark Results

Benchmarks run on ARM64 (Apple Silicon/Graviton-class) comparing Memento with system malloc, mimalloc, and rpmalloc:

```bash
cd bench
mkdir build && cd build
cmake ..
make -j
./benchmark_suite
```

#### Single-Threaded Performance (100K allocations + deallocations)

| Allocator | Small Fixed (64B) | Variable (16-256B) | Medium (4KB) |
|-----------|-------------------|-------------------|--------------|
| **mimalloc** | 12.4 Mops/s | 14.4 Mops/s | 8.9 Mops/s |
| **Memento** | 6.6 Mops/s | 7.7 Mops/s | 6.0 Mops/s |
| **rpmalloc** | 7.5 Mops/s | 8.2 Mops/s | 1.0 Mops/s |
| **System malloc** | 8.8 Mops/s | 9.1 Mops/s | 1.8 Mops/s |

*Higher is better. 1 Mops/s = 1 million operations per second.*

#### Key Observations

1. **mimalloc** leads in raw single-threaded performance (highly optimized)
2. **Memento** provides competitive performance with a simpler implementation
3. **Memento's** non-locking design scales linearly with thread count
4. **System malloc** varies significantly by platform (glibc, musl, etc.)

#### Scalability (Multi-Threaded)

| Threads | Memento | mimalloc | rpmalloc | malloc |
|---------|---------|----------|----------|--------|
| 1 | Baseline | Baseline | Baseline | Baseline |
| 4 | 4x | 4x | 4x | 1-2x |
| 8 | 8x | 8x | 8x | 1-2x |

*Memento, mimalloc, and rpmalloc all scale linearly due to thread-local designs. System malloc shows contention under thread pressure.*

## Design

### Non-Locking Thread-Local Design

```
Thread A Heap              Thread B Heap
+-----------------+        +-----------------+
| Size Class 0    |        | Size Class 0    |
|   Cache: 64 ptr |        |   Cache: 64 ptr |
| Size Class 1    |        | Size Class 1    |
|   Cache: 64 ptr |        |   Cache: 64 ptr |
|     ...         |        |     ...         |
| Foreign Free    |        | Foreign Free    |
|   Ring Buffer   |<-------|   Ring Buffer   |
+-----------------+        +-----------------+
```

Each thread has:
- **16 size class caches** (32B, 64B, 96B, 128B, 192B, 256B, 384B, 512B, 768B, 1KB, 1.5KB, 2KB, 3KB, 4KB, 6KB, 8KB)
- **Foreign-free ring buffer** (256 entries for cross-thread deallocation)
- **Statistics** (allocation counts, bytes used)

### Size Class Layout

| Size Class | Size | Usage |
|------------|------|-------|
| 0 | 32B | Tiny objects |
| 1 | 64B | Small strings, nodes |
| 2 | 96B | Medium structs |
| 3 | 128B | Common object size |
| 4 | 192B | Larger structs |
| 5 | 256B | Small buffers |
| 6 | 384B | Medium buffers |
| 7 | 512B | Network packets |
| 8 | 768B | Large structs |
| 9 | 1KB | Page-sized data |
| 10 | 1.5KB | Buffers |
| 11 | 2KB | Small arrays |
| 12 | 3KB | Medium arrays |
| 13 | 4KB | Page alignment |
| 14 | 6KB | Large buffers |
| 15 | 8KB | Maximum cached |

Allocations > 8KB go directly to the system.

## Building

### Header-Only

Just copy `include/memento.h` (and optionally `include/memento.hpp` for C++) to your project.

### With CMake

```bash
mkdir build && cd build
cmake ..
make -j
```

### Running Tests

```bash
# C tests
gcc -std=c99 -O2 -Iinclude tests/test_core.c -o test_core -lpthread
./test_core

# C++ tests
g++ -std=c++17 -O2 -Iinclude tests/test_cpp.cpp -o test_cpp -lpthread
./test_cpp

# Thread safety tests
gcc -std=c99 -O2 -Iinclude tests/test_thread.c -o test_thread -lpthread
./test_thread

# All tests via CMake
cmake -DBUILD_TESTING=ON ..
make -j
ctest --output-on-failure
```

## API Reference

### C API

```c
// Initialization
bool memento_init(void);
void memento_shutdown(void);

// Thread Heap
memento_thread_heap_t* memento_thread_heap_get(void);
void* memento_thread_heap_alloc(memento_thread_heap_t* heap, size_t size);
void memento_thread_heap_free(memento_thread_heap_t* heap, void* ptr, size_t size);
void* memento_thread_heap_realloc(memento_thread_heap_t* heap, void* ptr, 
                                   size_t old_size, size_t new_size);

// Pool
memento_pool_t* memento_pool_create(size_t object_size, size_t capacity, 
                                     memento_thread_heap_t* heap);
void* memento_pool_alloc(memento_pool_t* pool);
void memento_pool_free(memento_pool_t* pool, void* ptr);
void memento_pool_destroy(memento_pool_t* pool);

// Arena
memento_arena_t* memento_arena_create(size_t initial_capacity,
                                       memento_thread_heap_t* heap);
void* memento_arena_alloc(memento_arena_t* arena, size_t size, size_t alignment);
memento_arena_save_t memento_arena_save(memento_arena_t* arena);
void memento_arena_restore(memento_arena_t* arena, memento_arena_save_t* save);
void memento_arena_reset(memento_arena_t* arena);
void memento_arena_destroy(memento_arena_t* arena);

// Stack
memento_stack_t* memento_stack_create(size_t capacity, memento_thread_heap_t* heap);
void* memento_stack_push(memento_stack_t* stack, size_t size, size_t alignment);
memento_stack_marker_t memento_stack_marker(memento_stack_t* stack);
void memento_stack_pop_to_marker(memento_stack_t* stack, memento_stack_marker_t marker);
void memento_stack_reset(memento_stack_t* stack);
void memento_stack_destroy(memento_stack_t* stack);

// Slab
memento_slab_t* memento_slab_create(memento_thread_heap_t* heap);
void* memento_slab_alloc(memento_slab_t* slab, size_t size);
void memento_slab_free(memento_slab_t* slab, void* ptr, size_t size);
void memento_slab_destroy(memento_slab_t* slab);

// Utilities
size_t memento_size_class_for(size_t size);
size_t memento_size_class_to_size(size_t sc);
size_t memento_align_up(size_t size, size_t alignment);
```

### C++ API

```cpp
namespace memento {
    // Context
    class context;  // RAII initialization
    
    // Heap
    class heap {
        void* allocate(size_t size);
        void deallocate(void* ptr, size_t size);
        template<typename T, typename... Args> T* construct(Args&&... args);
        template<typename T> void destroy(T* ptr);
    };
    
    // Pool
    template<typename T>
    class pool {
        explicit pool(size_t capacity, heap* h = nullptr);
        template<typename... Args> T* emplace(Args&&... args);
        void destroy(T* ptr);
    };
    
    // Arena
    class arena {
        explicit arena(size_t initial_capacity, heap* h = nullptr);
        void* allocate(size_t size, size_t alignment = alignof(max_align_t));
        template<typename T, typename... Args> T* construct(Args&&... args);
        save_point save();
        void restore(const save_point& sp);
        void reset();
        size_t used() const;
        size_t capacity() const;
    };
    
    // Stack
    template<typename T>
    class stack {
        explicit stack(size_t capacity, heap* h = nullptr);
        template<typename... Args> T* push(Args&&... args);
        void pop(T* ptr);
        marker mark();
        void restore(const marker& m);
        void reset();
    };
    
    // STL Allocator
    template<typename T>
    class allocator {
        explicit allocator(heap& h);
        T* allocate(size_t n);
        void deallocate(T* ptr, size_t n);
    };
    
    // Scoped Pointer
    template<typename T>
    class scoped_ptr {
        explicit scoped_ptr(T* ptr, heap* h);
        T* get() const;
        void reset(T* ptr = nullptr);
        T* release();
    };
}
```

## Platform Support

| Platform | Compiler | Status |
|----------|----------|--------|
| Linux | GCC 9+ | Tested |
| Linux | Clang 10+ | Tested |
| macOS | Clang 12+ | Supported |
| Windows | MSVC 2019+ | Supported |
| Windows | MinGW-w64 | Supported |

## Contributing

Contributions are welcome! Please:
1. Run the test suite before submitting
2. Add tests for new features
3. Follow the existing code style
4. Update documentation

## License

MIT License - See [LICENSE](LICENSE) file

## Acknowledgments

- **rpmalloc** by Mattias Jansson - Design inspiration
- **mimalloc** by Microsoft Research - Comparison target
- **nanobench** by Martin Leitner-Ankerl - Benchmarking library

## Version History

### 2.0.0 (2024)
- Complete rewrite with non-locking design
- Single header layout
- C++17/20 support with concepts
- Multiple allocator strategies
- Comprehensive benchmark suite

### v1.x (Legacy)
- Original allocator with atomic operations
- Split header design
- See `v1` branch for old code
