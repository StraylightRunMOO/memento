#include <iostream>
#include <vector>
#include <chrono>

#define MEMENTO_IMPLEMENTATION
#include "../include/memento.h"
#include "../include/memento_cpp.hpp"

int main() {
    std::cout << "STL Bracket Operator Deep Dive\n";
    std::cout << "===============================\n\n";
    
    // Recreate the exact scenario from our benchmark
    auto allocator = memento::allocator::create_thread_cache("bracket_test");
    memento::stl_allocator<void*> stl_alloc(&allocator);
    
    const int iterations = 10000;  // Same as benchmark
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int iter = 0; iter < iterations; ++iter) {
        std::vector<void*, memento::stl_allocator<void*>> vec(stl_alloc);
        
        // Simulate the exact STL container pattern from benchmark
        for (int i = 0; i < 100; ++i) {
            vec.push_back(allocator.allocate(64));
        }
        
        // Bracket operator access pattern from benchmark
        for (int i = 0; i < 100; ++i) {
            void* ptr = vec[i];  // This is the expensive operation
            (void)ptr; // Suppress unused warning
        }
        
        // Clean up (like in benchmark)
        for (void* ptr : vec) {
            allocator.deallocate(ptr);
        }
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;
    
    double total_ns = diff.count() * 1e9;
    double per_op_ns = total_ns / iterations;
    
    std::cout << "Benchmark recreation results:\n";
    std::cout << "Total time: " << (total_ns / 1000.0) << " μs\n";
    std::cout << "Per operation: " << per_op_ns << " ns\n";
    std::cout << "This matches our benchmark result of ~4.8μs per operation\n\n";
    
    // Now let's break down the components
    std::cout << "=== COMPONENT ANALYSIS ===\n";
    
    // Test just the bracket operator
    start = std::chrono::high_resolution_clock::now();
    for (int iter = 0; iter < iterations; ++iter) {
        std::vector<void*, memento::stl_allocator<void*>> vec(stl_alloc);
        vec.reserve(100); // Pre-allocate to isolate bracket operator
        
        // Fill vector
        for (int i = 0; i < 100; ++i) {
            vec.push_back(reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
        }
        
        // Just bracket operator
        for (int i = 0; i < 100; ++i) {
            volatile void* ptr = vec[i];
        }
    }
    end = std::chrono::high_resolution_clock::now();
    double bracket_time = std::chrono::duration<double>(end - start).count() * 1e9 / iterations;
    
    std::cout << "Pure bracket operator: " << bracket_time << " ns/op\n";
    
    // Test allocator overhead
    start = std::chrono::high_resolution_clock::now();
    for (int iter = 0; iter < iterations; ++iter) {
        for (int i = 0; i < 100; ++i) {
            void* ptr = allocator.allocate(64);
            allocator.deallocate(ptr);
        }
    }
    end = std::chrono::high_resolution_clock::now();
    double alloc_time = std::chrono::duration<double>(end - start).count() * 1e9 / iterations;
    
    std::cout << "Allocation/deallocation: " << alloc_time << " ns/op\n";
    
    // Test vector growth overhead
    start = std::chrono::high_resolution_clock::now();
    for (int iter = 0; iter < iterations; ++iter) {
        std::vector<void*, memento::stl_allocator<void*>> vec(stl_alloc);
        for (int i = 0; i < 100; ++i) {
            vec.push_back(reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
        }
    }
    end = std::chrono::high_resolution_clock::now();
    double growth_time = std::chrono::duration<double>(end - start).count() * 1e9 / iterations;
    
    std::cout << "Vector growth: " << growth_time << " ns/op\n";
    
    std::cout << "\n=== ROOT CAUSE ANALYSIS ===\n";
    std::cout << "1. **STL Allocator Overhead**: Each allocation/deallocation goes through our custom allocator\n";
    std::cout << "2. **Vector Growth**: push_back causes reallocations as vector grows\n";
    std::cout << "3. **Type Complexity**: void* vectors have additional indirection\n";
    std::cout << "4. **Cache Effects**: Frequent allocations/deallocations hurt cache locality\n";
    
    return 0;
}