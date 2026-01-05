#!/bin/bash

# Build and test script for Memento allocator library

set -e  # Exit on any error

echo "=== Memento Allocator Build and Test Script ==="
echo

# Create build directory
mkdir -p build

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to print status
print_status() {
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✓ $1${NC}"
    else
        echo -e "${RED}✗ $1${NC}"
        exit 1
    fi
}

# Function to run a test
run_test() {
    local test_name=$1
    local test_cmd=$2
    
    echo -n "Running $test_name... "
    if eval "$test_cmd" > /dev/null 2>&1; then
        echo -e "${GREEN}PASSED${NC}"
        return 0
    else
        echo -e "${RED}FAILED${NC}"
        return 1
    fi
}

echo "Building C examples..."

# Build C examples
echo -n "Building example_basic... "
gcc -std=c99 -O2 -o build/example_basic examples/example_basic.c -lm
print_status "Built example_basic"

echo -n "Building example_thread_cache... "
gcc -std=c99 -O2 -o build/example_thread_cache examples/example_thread_cache.c -lm
print_status "Built example_thread_cache"

echo -n "Building example_block_allocator... "
gcc -std=c99 -O2 -o build/example_block_allocator examples/example_block_allocator.c -lm
print_status "Built example_block_allocator"

echo -n "Building example_proxy_allocator... "
gcc -std=c99 -O2 -o build/example_proxy_allocator examples/example_proxy_allocator.c -lm
print_status "Built example_proxy_allocator"

echo -n "Building example_stack_allocator... "
gcc -std=c99 -O2 -o build/example_stack_allocator examples/example_stack_allocator.c -lm
print_status "Built example_stack_allocator"

echo -n "Building example_hierarchy... "
gcc -std=c99 -O2 -o build/example_hierarchy examples/example_hierarchy.c -lm
print_status "Built example_hierarchy"

echo -n "Building example_statistics... "
gcc -std=c99 -O2 -o build/example_statistics examples/example_statistics.c -lm
print_status "Built example_statistics"

echo -n "Building example_alignment... "
gcc -std=c99 -O2 -o build/example_alignment examples/example_alignment.c -lm
print_status "Built example_alignment"

echo
echo "Building C++ examples..."

# Build C++ examples
echo -n "Building example_cpp_basic... "
g++ -std=c++17 -O2 -o build/example_cpp_basic examples/example_cpp_basic.cpp -lm
print_status "Built example_cpp_basic"

echo -n "Building example_cpp_arena... "
g++ -std=c++17 -O2 -o build/example_cpp_arena examples/example_cpp_arena.cpp -lm
print_status "Built example_cpp_arena"

echo -n "Building example_cpp_stl... "
g++ -std=c++17 -O2 -o build/example_cpp_stl examples/example_cpp_stl.cpp -lm
print_status "Built example_cpp_stl"

echo -n "Building example_cpp_custom... "
g++ -std=c++17 -O2 -o build/example_cpp_custom examples/example_cpp_custom.cpp -lm
print_status "Built example_cpp_custom"

echo -n "Building example_game_engine... "
g++ -std=c++17 -O2 -o build/example_game_engine examples/example_game_engine.cpp -lm
print_status "Built example_game_engine"

echo
echo "Building test suites..."

# Build test suites
echo -n "Building test_basic... "
gcc -std=c99 -O2 -o build/test_basic tests/test_basic.c -lm
print_status "Built test_basic"

echo -n "Building test_alignment... "
gcc -std=c99 -O2 -o build/test_alignment tests/test_alignment.c -lm
print_status "Built test_alignment"

echo -n "Building test_stress... "
gcc -std=c99 -O2 -o build/test_stress tests/test_stress.c -lm
print_status "Built test_stress"

echo -n "Building test_leaks... "
gcc -std=c99 -O2 -o build/test_leaks tests/test_leaks.c -lm
print_status "Built test_leaks"

echo
echo "Testing examples..."

# Test a few key examples
run_test "Basic Example" "./build/example_basic | grep -q 'completed successfully'"
run_test "Thread Cache Example" "./build/example_thread_cache | grep -q 'completed successfully'"
run_test "Block Allocator Example" "./build/example_block_allocator | grep -q 'completed successfully'"
run_test "C++ Basic Example" "./build/example_cpp_basic | grep -q 'completed successfully'"
run_test "C++ Arena Example" "./build/example_cpp_arena | grep -q 'completed successfully'"

echo
echo "Testing test suites..."

# Test suites
run_test "Basic Tests" "./build/test_basic | grep -q 'Failed: 0'"
run_test "Alignment Tests" "./build/test_alignment | grep -q 'Failed: 0'"
run_test "Stress Tests" "./build/test_stress | grep -q 'Failed: 0'"
run_test "Leak Detection Tests" "./build/test_leaks | grep -q 'Failed: 0'"

echo
echo "=== Build and Test Summary ==="
echo -e "${GREEN}All builds and tests completed successfully!${NC}"
echo
echo "Built examples:"
ls -1 build/example_* | sed 's|build/||g' | sed 's|^|  |'
echo
echo "Built tests:"
ls -1 build/test_* | sed 's|build/||g' | sed 's|^|  |'
echo
echo "To run all examples:"
echo "  for exe in build/example_*; do echo \"Running \$exe\"; \"\$exe\"; echo; done"
echo
echo "To run all tests:"
echo "  for exe in build/test_*; do echo \"Running \$exe\"; \"\$exe\"; echo; done"