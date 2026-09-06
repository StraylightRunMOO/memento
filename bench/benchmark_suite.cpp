/*
 * Memento Benchmark Suite
 * 
 * Comprehensive benchmarks comparing Memento with:
 * - System malloc/free
 * - mimalloc
 * - rpmalloc
 */

#include <iostream>
#include <iomanip>
#include <vector>
#include <chrono>
#include <random>
#include <algorithm>
#include <cstring>

// Memento
#define MEMENTO_IMPLEMENTATION
#include "memento.h"

// External allocators
#include "mimalloc.h"
#include "rpmalloc.h"

// Simple benchmark harness
struct BenchmarkResult {
    const char* name;
    double ops_per_sec;
    double ns_per_op;
};

void print_header(const char* title) {
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "  " << title << "\n";
    std::cout << std::string(70, '=') << "\n";
}

void print_subheader(const char* title) {
    std::cout << "\n  " << title << "\n";
    std::cout << "  " << std::string(60, '-') << "\n";
}

template<typename AllocFn, typename FreeFn>
void run_benchmark(const char* name, int iterations, size_t size, AllocFn alloc, FreeFn free_fn) {
    std::vector<void*> ptrs;
    ptrs.reserve(iterations);
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // Allocate
    for (int i = 0; i < iterations; i++) {
        ptrs.push_back(alloc(size));
    }
    // Free (in reverse order to stress cache)
    for (int i = iterations - 1; i >= 0; i--) {
        free_fn(ptrs[i]);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
    double ops_per_sec = (iterations * 1000.0) / ms;
    double ns_per_op = (ms * 1000000.0) / iterations;
    
    std::cout << "  " << std::setw(18) << std::left << name 
              << ": " << std::setw(8) << std::fixed << std::setprecision(1) << ops_per_sec / 1000000.0 
              << " Mops/sec (" << std::setw(6) << std::setprecision(1) << ns_per_op << " ns/op)\n";
}

// Memento heap shared by the benchmark lambdas
memento_thread_heap_t* g_memento_heap = nullptr;

int main() {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║          MEMENTO BENCHMARK SUITE                                     ║\n";
    std::cout << "║          Comparing: Memento, mimalloc, rpmalloc, system malloc       ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════════╝\n";
    
    // Initialize allocators
    memento_init();
    g_memento_heap = memento_thread_heap_get();
    rpmalloc_initialize();
    
    print_header("SINGLE-THREADED BENCHMARKS");
    
    // Small fixed-size allocations
    print_subheader("Small Fixed-Size (64 bytes, 100K ops)");
    run_benchmark("System Malloc", 100000, 64, 
        [](size_t s){ return malloc(s); },
        [](void* p){ free(p); });
    
    run_benchmark("Memento", 100000, 64,
        [](size_t s){ return memento_thread_heap_alloc(g_memento_heap, s); },
        [](void* p){ if(p) memento_thread_heap_free(g_memento_heap, p, 64); });
    
    run_benchmark("mimalloc", 100000, 64,
        [](size_t s){ return mi_malloc(s); },
        [](void* p){ mi_free(p); });
    
    run_benchmark("rpmalloc", 100000, 64,
        [](size_t s){ return rpmalloc(s); },
        [](void* p){ rpfree(p); });
    
    // Variable small allocations
    print_subheader("Variable Small (16-256 bytes, 50K ops)");
    {
        const int N = 50000;
        size_t sizes[N];
        std::mt19937 rng(42);
        std::uniform_int_distribution<size_t> dist(16, 256);
        for (int i = 0; i < N; i++) sizes[i] = dist(rng);
        
        // System
        {
            std::vector<void*> ptrs; ptrs.reserve(N);
            auto start = std::chrono::high_resolution_clock::now();
            for (int i = 0; i < N; i++) ptrs.push_back(malloc(sizes[i]));
            for (int i = N-1; i >= 0; i--) free(ptrs[i]);
            auto end = std::chrono::high_resolution_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
            double ops = (N * 1000.0) / ms;
            std::cout << "  " << std::setw(18) << std::left << "System Malloc" 
                      << ": " << std::setw(8) << std::fixed << std::setprecision(1) << ops / 1000000.0 
                      << " Mops/sec\n";
        }
        
        // Memento
        {
            std::vector<void*> ptrs; ptrs.reserve(N);
            auto start = std::chrono::high_resolution_clock::now();
            for (int i = 0; i < N; i++) ptrs.push_back(memento_thread_heap_alloc(g_memento_heap, sizes[i]));
            for (int i = N-1; i >= 0; i--) memento_thread_heap_free(g_memento_heap, ptrs[i], sizes[i]);
            auto end = std::chrono::high_resolution_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
            double ops = (N * 1000.0) / ms;
            std::cout << "  " << std::setw(18) << std::left << "Memento" 
                      << ": " << std::setw(8) << std::fixed << std::setprecision(1) << ops / 1000000.0 
                      << " Mops/sec\n";
        }
        
        // mimalloc
        {
            std::vector<void*> ptrs; ptrs.reserve(N);
            auto start = std::chrono::high_resolution_clock::now();
            for (int i = 0; i < N; i++) ptrs.push_back(mi_malloc(sizes[i]));
            for (int i = N-1; i >= 0; i--) mi_free(ptrs[i]);
            auto end = std::chrono::high_resolution_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
            double ops = (N * 1000.0) / ms;
            std::cout << "  " << std::setw(18) << std::left << "mimalloc" 
                      << ": " << std::setw(8) << std::fixed << std::setprecision(1) << ops / 1000000.0 
                      << " Mops/sec\n";
        }
        
        // rpmalloc
        {
            std::vector<void*> ptrs; ptrs.reserve(N);
            auto start = std::chrono::high_resolution_clock::now();
            for (int i = 0; i < N; i++) ptrs.push_back(rpmalloc(sizes[i]));
            for (int i = N-1; i >= 0; i--) rpfree(ptrs[i]);
            auto end = std::chrono::high_resolution_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
            double ops = (N * 1000.0) / ms;
            std::cout << "  " << std::setw(18) << std::left << "rpmalloc" 
                      << ": " << std::setw(8) << std::fixed << std::setprecision(1) << ops / 1000000.0 
                      << " Mops/sec\n";
        }
    }
    
    // Medium allocations
    print_subheader("Medium (4 KB, 10K ops)");
    run_benchmark("System Malloc", 10000, 4096, 
        [](size_t s){ return malloc(s); },
        [](void* p){ free(p); });
    
    run_benchmark("Memento", 10000, 4096,
        [](size_t s){ return memento_thread_heap_alloc(g_memento_heap, s); },
        [](void* p){ if(p) memento_thread_heap_free(g_memento_heap, p, 4096); });
    
    run_benchmark("mimalloc", 10000, 4096,
        [](size_t s){ return mi_malloc(s); },
        [](void* p){ mi_free(p); });
    
    run_benchmark("rpmalloc", 10000, 4096,
        [](size_t s){ return rpmalloc(s); },
        [](void* p){ rpfree(p); });
    
    // Cleanup
    rpmalloc_finalize();
    memento_shutdown();
    
    // Summary
    print_header("BENCHMARK SUMMARY");
    std::cout << "\n  Notes:\n";
    std::cout << "  • Benchmarks run on a single thread; numbers vary with CPU and load\n";
    std::cout << "  • Memento's hot path touches one cache line (freelist head) plus\n";
    std::cout << "    the span header's live counter; no atomics, no locks\n";
    std::cout << "  • Empty spans are parked, not discarded: pages return to the kernel\n";
    std::cout << "    only after MEMENTO_SPAN_PURGE_MS idle (10 ms default), so burst\n";
    std::cout << "    traffic never pays the madvise ping-pong\n\n";
    
    return 0;
}
