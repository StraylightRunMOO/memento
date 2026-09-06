cmake_minimum_required(VERSION 3.14)
project(memento_benchmarks VERSION 3.0.0 LANGUAGES C CXX)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
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

# compare.c: head-to-head harness, one binary per allocator (see file header
# for methodology and hand-built invocation examples).
# Always -O3 -march=native: an empty CMAKE_BUILD_TYPE otherwise ships
# unoptimized compare binaries and the table is meaningless.
set(MEMENTO_COMPARE_C_FLAGS)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    list(APPEND MEMENTO_COMPARE_C_FLAGS -O3 -DNDEBUG -march=native)
endif()

foreach(which memento memento_nostats mimalloc rpmalloc glibc)
    add_executable(compare_${which} compare.c)
    set_property(TARGET compare_${which} PROPERTY C_STANDARD 11)
    target_link_libraries(compare_${which} pthread)
    if(MEMENTO_COMPARE_C_FLAGS)
        target_compile_options(compare_${which} PRIVATE ${MEMENTO_COMPARE_C_FLAGS})
    endif()
endforeach()
target_compile_definitions(compare_memento PRIVATE ALLOC_MIMENTO3)
target_include_directories(compare_memento PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_definitions(compare_memento_nostats PRIVATE ALLOC_MIMENTO3 MEMENTO_STATS=0)
target_include_directories(compare_memento_nostats PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_definitions(compare_mimalloc PRIVATE ALLOC_MIMALLOC)
target_include_directories(compare_mimalloc PRIVATE ${mimalloc_SOURCE_DIR}/include)
target_link_libraries(compare_mimalloc mimalloc-static)
target_compile_definitions(compare_rpmalloc PRIVATE ALLOC_RPMALLOC)
target_include_directories(compare_rpmalloc PRIVATE ${rpmalloc_SOURCE_DIR}/rpmalloc)
target_link_libraries(compare_rpmalloc rpmalloc_lib)
# compare_glibc needs no define: system malloc is the default.

if(MEMENTO_COMPARE_C_FLAGS)
    target_compile_options(benchmark_suite PRIVATE ${MEMENTO_COMPARE_C_FLAGS})
    if(TARGET mimalloc-static)
        target_compile_options(mimalloc-static PRIVATE ${MEMENTO_COMPARE_C_FLAGS})
    endif()
    target_compile_options(rpmalloc_lib PRIVATE ${MEMENTO_COMPARE_C_FLAGS})
endif()

add_custom_target(run_compare
    COMMAND compare_glibc
    COMMAND compare_mimalloc
    COMMAND compare_rpmalloc
    COMMAND compare_memento
    COMMAND compare_memento_nostats
    DEPENDS compare_glibc compare_mimalloc compare_rpmalloc compare_memento compare_memento_nostats
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running head-to-head allocator comparison..."
)
