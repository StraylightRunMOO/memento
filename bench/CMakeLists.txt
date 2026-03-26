cmake_minimum_required(VERSION 3.14)
project(memento_benchmarks VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include FetchContent
include(FetchContent)

# nanobench for benchmarking (header-only)
FetchContent_Declare(
    nanobench
    GIT_REPOSITORY https://github.com/martinus/nanobench.git
    GIT_TAG v4.3.11
)
FetchContent_MakeAvailable(nanobench)

# mimalloc for comparison
set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    mimalloc
    GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
    GIT_TAG v2.1.7
)
FetchContent_MakeAvailable(mimalloc)

# rpmalloc for comparison
FetchContent_Declare(
    rpmalloc
    GIT_REPOSITORY https://github.com/mjansson/rpmalloc.git
    GIT_TAG 1.4.5
)
FetchContent_MakeAvailable(rpmalloc)

# Build rpmalloc as static library
add_library(rpmalloc_lib STATIC
    ${rpmalloc_SOURCE_DIR}/rpmalloc/rpmalloc.c
    ${rpmalloc_SOURCE_DIR}/rpmalloc/rpmalloc.h
)
target_include_directories(rpmalloc_lib PUBLIC ${rpmalloc_SOURCE_DIR})
set_property(TARGET rpmalloc_lib PROPERTY C_STANDARD 11)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rpmalloc_lib PRIVATE -Wno-c11-extensions)
endif()

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${nanobench_SOURCE_DIR}/src/include
    ${mimalloc_SOURCE_DIR}/include
    ${rpmalloc_SOURCE_DIR}/rpmalloc
)

# Main benchmark executable
add_executable(benchmark_suite
    benchmark_suite.cpp
)

target_link_libraries(benchmark_suite
    nanobench
    mimalloc-static
    rpmalloc_lib
)

# Platform-specific linking
if(WIN32)
    target_link_libraries(benchmark_suite psapi)
elseif(APPLE)
    target_link_libraries(benchmark_suite pthread)
else()
    target_link_libraries(benchmark_suite pthread dl)
endif()

# Custom target to run benchmarks
add_custom_target(run_benchmarks
    COMMAND benchmark_suite
    DEPENDS benchmark_suite
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running comprehensive benchmark suite..."
)

# Install target
install(TARGETS benchmark_suite DESTINATION bin)
