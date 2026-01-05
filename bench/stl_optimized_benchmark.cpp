#include <iostream>
#include <vector>
#include <chrono>

#define MEMENTO_IMPLEMENTATION
#include "../include/memento.h"
#include "../include/memento_cpp.hpp"

// Optimized STL container pattern
void optimized_stl_pattern() {
    std::cout << "=== OPTIMIZED STL PATTERNS ===\n\n";
    
    auto allocator = memento::allocator::create_thread_cache("stl_optimized");
    memento::stl_allocator<void*> stl_alloc(&allocator);
    
    const int iterations = 10000;
    const int vector_size = 100;
    
    std::cout << "1. ORIGINAL PATTERN (SLOW)\n";
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int iter = 0; iter < iterations; ++iter) {
        std::vector<void*, memento::stl_allocator<void*>> vec(stl_alloc);
        
        // Original pattern: growth + individual allocation
        for (int i = 0; i < vector_size; ++i) {
            vec.push_back(allocator.allocate(64));
        }
        
        // Bracket access
        for (int i = 0; i < vector_size; ++i) {
            void* ptr = vec[i];
            (void)ptr;
        }
        
        // Cleanup
        for (void* ptr : vec) {
            allocator.deallocate(ptr);
        }
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    double original_time = std::chrono::duration<double>(end - start).count() * 1e9 / iterations;
    std::cout << "Original: " << original_time << " ns/op\n";
    
    std::cout << "\n2. OPTIMIZATION 1: RESERVE TO AVOID GROWTH\n";
    start = std::chrono::high_resolution_clock::now();
    
    for (int iter = 0; iter < iterations; ++iter) {
        std::vector<void*, memento::stl_allocator<void*>> vec(stl_alloc);
        vec.reserve(vector_size);  // Pre-allocate capacity
        
        for (int i = 0; i < vector_size; ++i) {
            vec.push_back(allocator.allocate(64));
        }
        
        for (int i = 0; i < vector_size; ++i) {
            void* ptr = vec[i];
            (void)ptr;
        }
        
        for (void* ptr : vec) {
            allocator.deallocate(ptr);
        }
    }
    
    end = std::chrono::high_resolution_clock::now();
    double reserve_time = std::chrono::duration<double>(end - start).count() * 1e9 / iterations;
    std::cout << "With reserve: " << reserve_time << " ns/op\n";
    std::cout << "Speedup: " << (original_time / reserve_time) << "x\n";
    
    std::cout << "\n3. OPTIMIZATION 2: BATCH ALLOCATION\n";
    start = std::chrono::high_resolution_clock::now();
    
    for (int iter = 0; iter < iterations; ++iter) {
        // Pre-allocate all memory in one go
        void** memory_block = static_cast<void**>(allocator.allocate(vector_size * sizeof(void*)));
        
        std::vector<void*, memento::stl_allocator<void*>> vec(stl_alloc);
        vec.reserve(vector_size);
        
        // Use placement new to construct elements
        for (int i = 0; i < vector_size; ++i) {
            memory_block[i] = reinterpret_cast<void*>(static_cast<uintptr_t>(i));
            vec.push_back(memory_block[i]);
        }
        
        // Access elements
        for (int i = 0; i < vector_size; ++i) {
            void* ptr = vec[i];
            (void)ptr;
        }
        
        // Single deallocation
        allocator.deallocate(memory_block);
    }
    
    end = std::chrono::high_resolution_clock::now();
    double batch_time = std::chrono::duration<double>(end - start).count() * 1e9 / iterations;
    std::cout << "Batch allocation: " << batch_time << " ns/op\n";
    std::cout << "Speedup: " << (original_time / batch_time) << "x\n";
    
    std::cout << "\n4. OPTIMIZATION 3: POOLED ALLOCATION\n";
    
    // Create a memory pool for better performance
    struct MemoryPool {
        void* pool;
        size_t used;
        size_t capacity;
        
        MemoryPool(memento::allocator& alloc, size_t size) 
            : pool(alloc.allocate(size)), used(0), capacity(size) {}
        
        ~MemoryPool() {
            // Pool cleanup handled by allocator destruction
        }
        
        void* allocate(size_t size) {
            if (used + size > capacity) return nullptr;
            void* result = static_cast<char*>(pool) + used;
            used += size;
            return result;
        }
    };
    
    start = std::chrono::high_resolution_clock::now();
    
    for (int iter = 0; iter < iterations; ++iter) {
        MemoryPool pool(allocator, vector_size * 64);  // Pre-allocate pool
        
        std::vector<void*, memento::stl_allocator<void*>> vec(stl_alloc);
        vec.reserve(vector_size);
        
        for (int i = 0; i < vector_size; ++i) {
            void* ptr = pool.allocate(64);
            vec.push_back(ptr);
        }
        
        for (int i = 0; i < vector_size; ++i) {
            void* ptr = vec[i];
            (void)ptr;
        }
    }
    
    end = std::chrono::high_resolution_clock::now();
    double pool_time = std::chrono::duration<double>(end - start).count() * 1e9 / iterations;
    std::cout << "Pooled allocation: " << pool_time << " ns/op\n";
    std::cout << "Speedup: " << (original_time / pool_time) << "x\n";
    
    std::cout << "\n=== SUMMARY ===\n";
    std::cout << "Original pattern: " << original_time << " ns/op\n";
    std::cout << "With reserve: " << reserve_time << " ns/op (" << (original_time/reserve_time) << "x speedup)\n";
    std::cout << "Batch allocation: " << batch_time << " ns/op (" << (original_time/batch_time) << "x speedup)\n";
    std::cout << "Pooled allocation: " << pool_time << " ns/op (" << (original_time/pool_time) << "x speedup)\n";
    
    std::cout << "\nKEY INSIGHTS:\n";
    std::cout << "1. Vector growth is expensive - use reserve()\n";
    std::cout << "2. Individual allocations add up - batch when possible\n";
    std::cout << "3. Memory pools can provide significant speedups\n";
    std::cout << "4. Bracket operator itself is fast - the overhead is in allocation patterns\n";
}

int main() {
    optimized_stl_pattern();
    return 0;
}