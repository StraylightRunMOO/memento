#!/bin/bash

# Build script for comprehensive benchmarks
set -e

echo "Building Memento Allocator Comprehensive Benchmark Suite..."
echo "=========================================================="

# Create build directory
mkdir -p build
cd build

# Configure with CMake
echo "Configuring with CMake..."
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build
echo "Building benchmarks..."
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

echo ""
echo "✓ Build completed successfully!"
echo ""
echo "To run benchmarks:"
echo "  cd build && ./benchmark_all"
echo ""
echo "Or use the convenience target:"
echo "  make run_benchmarks"