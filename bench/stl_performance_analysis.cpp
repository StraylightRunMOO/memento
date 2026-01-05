#include <iostream>
#include <vector>
#include <chrono>
#include <numeric>
#include <algorithm>

#define MEMENTO_IMPLEMENTATION
#include "../include/memento.h"
#include "../include/memento_cpp.hpp"

template<typename Func>
double benchmark(const std::string& name, Func func, int iterations = 100000) {
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < iterations; ++i) {
        func();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;
    
    double ns_per_op = (diff.count() * 1e9) / iterations;
    std::cout << name << ": " << ns_per_op << " ns/op (" << (1000.0 / ns_per_op) << " M ops/sec)" << std::endl;
    
    return ns_per_op;
}

int main() {
    std::cout << "STL Vector Performance Analysis\n";
    std::cout << "================================\n\n";
    
    // Test different scenarios
    const int VECTOR_SIZE = 1000;
    const int ITERATIONS = 10000;
    
    // 1. Standard allocator baseline
    std::cout << "1. STANDARD ALLOCATOR BASELINE\n";
    benchmark("std::vector<int> bracket access", [VECTOR_SIZE]() {
        std::vector<int> vec(VECTOR_SIZE);
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            vec[i] = i;
        }
        volatile int sum = 0;
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            sum += vec[i];
        }
    }, ITERATIONS);
    
    // 2. Memento STL allocator
    std::cout << "\n2. MEMENTO STL ALLOCATOR\n";
    auto allocator = memento::allocator::create_thread_cache("stl_analysis");
    memento::stl_allocator<int> stl_alloc(&allocator);
    
    benchmark("memento::stl_allocator<int> bracket access", [&stl_alloc, VECTOR_SIZE]() {
        std::vector<int, memento::stl_allocator<int>> vec(VECTOR_SIZE, stl_alloc);
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            vec[i] = i;
        }
        volatile int sum = 0;
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            sum += vec[i];
        }
    }, ITERATIONS);
    
    // 3. Test with void* (like in benchmark)
    std::cout << "\n3. VOID* VECTOR (LIKE BENCHMARK)\n";
    memento::stl_allocator<void*> ptr_alloc(&allocator);
    
    benchmark("memento::stl_allocator<void*> bracket access", [&ptr_alloc, VECTOR_SIZE]() {
        std::vector<void*, memento::stl_allocator<void*>> vec(ptr_alloc);
        vec.reserve(VECTOR_SIZE);  // Pre-allocate to avoid growth overhead
        
        // Fill with dummy pointers
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            vec.push_back(reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
        }
        
        volatile void* sum = nullptr;
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            sum = vec[i];
        }
    }, ITERATIONS);
    
    // 4. Test allocation overhead separately
    std::cout << "\n4. ALLOCATION OVERHEAD ANALYSIS\n";
    
    benchmark("std::vector growth (standard)", [VECTOR_SIZE]() {
        std::vector<int> vec;
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            vec.push_back(i);
        }
    }, ITERATIONS);
    
    benchmark("memento::vector growth", [&stl_alloc, VECTOR_SIZE]() {
        std::vector<int, memento::stl_allocator<int>> vec(stl_alloc);
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            vec.push_back(i);
        }
    }, ITERATIONS);
    
    // 5. Test with pre-allocated capacity
    std::cout << "\n5. PRE-ALLOCATED CAPACITY COMPARISON\n";
    
    benchmark("std::vector with reserve", [VECTOR_SIZE]() {
        std::vector<int> vec;
        vec.reserve(VECTOR_SIZE);
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            vec.push_back(i);
        }
    }, ITERATIONS);
    
    benchmark("memento::vector with reserve", [&stl_alloc, VECTOR_SIZE]() {
        std::vector<int, memento::stl_allocator<int>> vec(stl_alloc);
        vec.reserve(VECTOR_SIZE);
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            vec.push_back(i);
        }
    }, ITERATIONS);
    
    std::cout << "\n=== ANALYSIS COMPLETE ===\n";
    std::cout << "Check the timing differences above to identify bottlenecks.\n";
    
    return 0;
}