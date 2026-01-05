cmake_minimum_required(VERSION 3.14)
project(memento_benchmarks)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include FetchContent for external dependencies
include(FetchContent)

# nanobench for benchmarking
FetchContent_Declare(
    nanobench
    GIT_REPOSITORY https://github.com/martinus/nanobench.git
    GIT_TAG v4.3.11
)
FetchContent_MakeAvailable(nanobench)

# mimalloc for comparison
FetchContent_Declare(
    mimalloc
    GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
    GIT_TAG v2.1.2
)
FetchContent_MakeAvailable(mimalloc)

# rpmalloc for comparison  
FetchContent_Declare(
    rpmalloc
    GIT_REPOSITORY https://github.com/mjansson/rpmalloc.git
    GIT_TAG 1.4.4
)
FetchContent_MakeAvailable(rpmalloc)

# rpmalloc needs to be built as a library
add_library(rpmalloc STATIC
    ${rpmalloc_SOURCE_DIR}/rpmalloc/rpmalloc.c
    ${rpmalloc_SOURCE_DIR}/rpmalloc/rpmalloc.h
)
target_include_directories(rpmalloc PUBLIC ${rpmalloc_SOURCE_DIR})
# rpmalloc requires C11 and generates warnings we need to suppress
set_property(TARGET rpmalloc PROPERTY C_STANDARD 11)
target_compile_options(rpmalloc PRIVATE 
    -Wno-c11-extensions 
    -Wno-static-in-inline 
    -Wno-excess-initializers
)

# Add include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/../include
    ${nanobench_SOURCE_DIR}/src/include
    ${mimalloc_SOURCE_DIR}/include
    ${rpmalloc_SOURCE_DIR}/include
)

# Add benchmark executable
add_executable(benchmark_all
    benchmark_all.cpp
)

# Link libraries
target_link_libraries(benchmark_all
    nanobench
    mimalloc-static
    rpmalloc
)

# Platform-specific linking
if(WIN32)
    target_link_libraries(benchmark_all psapi)
elseif(APPLE)
    target_link_libraries(benchmark_all pthread)
else()
    target_link_libraries(benchmark_all pthread dl)
endif()

# Add custom target to build and run benchmarks
add_custom_target(run_benchmarks
    COMMAND benchmark_all
    DEPENDS benchmark_all
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running comprehensive benchmarks..."
)