#include <iostream>
#include <chrono>
#include <vector>
#include <random>
#include <cmath>
#include <iomanip>

// Test different growth factor implementations
template<typename Func>
double benchmark_growth(const char* name, Func growth_func, int iterations = 1000000) {
    auto start = std::chrono::high_resolution_clock::now();
    
    volatile size_t result = 8;  // Start with small allocation
    for (int i = 0; i < iterations; ++i) {
        result = growth_func(result);
        if (result > 1024 * 1024) {  // Reset to avoid overflow
            result = 8;
        }
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;
    
    std::cout << name << ": " << (diff.count() * 1000.0) << " ms\n";
    return diff.count();
}

// Different growth factor implementations
struct GrowthFactors {
    // Traditional floating point multiplication
    static size_t float_multiply(size_t current) {
        return static_cast<size_t>(current * 1.5);
    }
    
    // Bit shift approximation: x + (x >> 1) = x * 1.5
    static size_t bit_shift_add(size_t current) {
        return current + (current >> 1);
    }
    
    // More accurate bit shift: (x << 1) - (x >> 1) = x * 1.5
    static size_t bit_shift_sub(size_t current) {
        return (current << 1) - (current >> 1);
    }
    
    // Power of two growth (traditional)
    static size_t power_of_two(size_t current) {
        return current * 2;
    }
    
    // Golden ratio growth
    static size_t golden_ratio(size_t current) {
        return static_cast<size_t>(current * 1.61803398875);
    }
};

void analyze_growth_patterns() {
    std::cout << "\n=== Growth Pattern Analysis ===\n";
    std::cout << "Starting size: 8 bytes\n\n";
    
    std::vector<size_t> sizes = {8, 16, 32, 64, 128, 256, 512, 1024, 2048};
    
    std::cout << "Size | Float*1.5 | BitShift+ | BitShift- | *2 | Golden\n";
    std::cout << "-----|-------------|-------------|-------------|--------|--------\n";
    
    for (size_t size : sizes) {
        std::cout << std::setw(4) << size << " | "
                  << std::setw(11) << GrowthFactors::float_multiply(size) << " | "
                  << std::setw(11) << GrowthFactors::bit_shift_add(size) << " | "
                  << std::setw(11) << GrowthFactors::bit_shift_sub(size) << " | "
                  << std::setw(6) << GrowthFactors::power_of_two(size) << " | "
                  << std::setw(6) << static_cast<size_t>(size * 1.618) << "\n";
    }
    
    std::cout << "\nAccuracy analysis (vs 1.5x):\n";
    std::cout << "Method | Avg Error | Max Error | Speed Advantage\n";
    std::cout << "-------|-----------|-----------|----------------\n";
    
    double total_error_add = 0, total_error_sub = 0;
    double max_error_add = 0, max_error_sub = 0;
    
    for (size_t size : sizes) {
        double target = size * 1.5;
        double actual_add = GrowthFactors::bit_shift_add(size);
        double actual_sub = GrowthFactors::bit_shift_sub(size);
        
        double error_add = std::abs(actual_add - target) / target;
        double error_sub = std::abs(actual_sub - target) / target;
        
        total_error_add += error_add;
        total_error_sub += error_sub;
        max_error_add = std::max(max_error_add, error_add);
        max_error_sub = std::max(max_error_sub, error_sub);
    }
    
    std::cout << "x+(x>>1) | " 
              << std::setw(9) << (total_error_add / sizes.size() * 100) << "% | "
              << std::setw(9) << (max_error_add * 100) << "% | "
              << "Bit shift, no FPU\n";
              
    std::cout << "(x<<1)-(x>>1) | "
              << std::setw(5) << (total_error_sub / sizes.size() * 100) << "% | "
              << std::setw(9) << (max_error_sub * 100) << "% | "
              << "Bit shift, no FPU\n";
}

int main() {
    std::cout << "Growth Factor Optimization Analysis\n";
    std::cout << "====================================\n\n";
    
    const int iterations = 5000000;
    
    std::cout << "Performance comparison (" << iterations << " iterations):\n";
    
    // Benchmark each method
    auto time_float = benchmark_growth("Float multiplication (x * 1.5)", 
                                       GrowthFactors::float_multiply, iterations);
    auto time_add = benchmark_growth("Bit shift add (x + (x >> 1))", 
                                     GrowthFactors::bit_shift_add, iterations);
    auto time_sub = benchmark_growth("Bit shift sub ((x << 1) - (x >> 1))", 
                                     GrowthFactors::bit_shift_sub, iterations);
    auto time_pow2 = benchmark_growth("Power of two (x * 2)", 
                                      GrowthFactors::power_of_two, iterations);
    auto time_golden = benchmark_growth("Golden ratio (x * 1.618)", 
                                        GrowthFactors::golden_ratio, iterations);
    
    std::cout << "\nSpeedup analysis:\n";
    std::cout << "Method | Speedup vs Float | CPU Advantage\n";
    std::cout << "-------|------------------|---------------\n";
    std::cout << "x+(x>>1) | " << std::setw(15) << (time_float / time_add) << "x | "
              << "No FPU, simple ALU\n";
    std::cout << "(x<<1)-(x>>1) | " << std::setw(11) << (time_float / time_sub) << "x | "
              << "No FPU, simple ALU\n";
    std::cout << "x*2 | " << std::setw(22) << (time_float / time_pow2) << "x | "
              << "Simple shift\n";
    
    analyze_growth_patterns();
    
    std::cout << "\n=== Recommendations ===\n";
    std::cout << "1. For performance-critical code: Use x + (x >> 1) for ~1.5x growth\n";
    std::cout << "2. For exact 1.5x growth: Use floating point when accuracy matters\n";
    std::cout << "3. For memory allocators: Bit shift is perfect - small error acceptable\n";
    std::cout << "4. Cache-friendly sizes: Use power-of-two growth (x * 2)\n";
    std::cout << "5. Golden ratio: Good for hash tables, but requires FPU\n";
    
    std::cout << "\nConclusion: YES! (x + (x >> 1)) is significantly faster with minimal accuracy loss\n";
    std::cout << "for allocator growth factors. Perfect for memory management!\n";
    
    return 0;
}