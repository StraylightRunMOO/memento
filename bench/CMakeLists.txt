# Benchmarks CMakeLists.txt

# Basic benchmarks
add_executable(bench_allocation bench_allocation.c)
target_link_libraries(bench_allocation m)

add_executable(bench_throughput bench_throughput.c)
target_link_libraries(bench_throughput m)

add_executable(bench_fragmentation bench_fragmentation.c)
target_link_libraries(bench_fragmentation m)

# C++ benchmarks
add_executable(bench_cpp_basic bench_cpp_basic.cpp)
target_link_libraries(bench_cpp_basic m)

add_executable(bench_cpp_stl bench_cpp_stl.cpp)
target_link_libraries(bench_cpp_stl m)

# Threading benchmarks (if available)
find_package(Threads)
if(Threads_FOUND)
    add_executable(bench_threads bench_threads.c)
    target_link_libraries(bench_threads m Threads::Threads)
endif()

# Performance comparison vs system malloc
add_executable(bench_comparison bench_comparison.c)
target_link_libraries(bench_comparison m)

# Set benchmark properties
set_target_properties(
    bench_allocation bench_throughput bench_fragmentation
    bench_cpp_basic bench_cpp_stl bench_threads bench_comparison
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bench"
)