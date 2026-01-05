# Makefile for Memento Allocator Benchmarks

CXX = g++
CXXFLAGS = -std=c++17 -O3 -march=native -Wall -Wextra
INCLUDES = -I../include -I.
LIBS = -lpthread -ldl

# Default target
all: growth_test

# Growth factor analysis
growth_test: growth_factor_test.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@ $(LIBS)
	@echo "Running growth factor analysis..."
	@./$@

# Quick benchmark without external dependencies
quick_bench: growth_test

# Full comprehensive benchmark (requires CMake and dependencies)
full_bench:
	@echo "Building comprehensive benchmarks with external allocators..."
	@./build_bench.sh

# Clean
clean:
	rm -f growth_test
	rm -rf build/

# Run all benchmarks
run: full_bench
	cd build && ./benchmark_all

.PHONY: all quick_bench full_bench clean run