cmake_minimum_required(VERSION 3.14)
project(memento VERSION 3.0.0)
    DESCRIPTION "High-performance non-locking memory allocator"
    LANGUAGES C CXX)

# C11 required for _Atomic / stdatomic.h (MSVC C uses Interlocked instead)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options
option(BUILD_TESTING "Build tests" ON)
option(BUILD_BENCHMARKS "Build benchmarks" OFF)
option(BUILD_EXAMPLES "Build examples" ON)

# Threads (pthread on POSIX; Windows threads via Win32 API in memento.h)
find_package(Threads REQUIRED)

# Main library target (header-only)
add_library(memento INTERFACE)
target_include_directories(memento INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(memento INTERFACE Threads::Threads)

# Testing
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

# Examples
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Benchmarks
if(BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()

# Installation
install(FILES
    include/memento.h
    include/memento.hpp
    DESTINATION include
)

install(TARGETS memento
    EXPORT memento-targets
    INCLUDES DESTINATION include
)

install(EXPORT memento-targets
    FILE memento-targets.cmake
    NAMESPACE memento::
    DESTINATION lib/cmake/memento
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_BINARY_DIR}/memento-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_BINARY_DIR}/memento-config-version.cmake"
    DESTINATION lib/cmake/memento
)

message(STATUS "")
message(STATUS "Memento ${PROJECT_VERSION} Configuration:")
message(STATUS "  Build tests: ${BUILD_TESTING}")
message(STATUS "  Build benchmarks: ${BUILD_BENCHMARKS}")
message(STATUS "  Build examples: ${BUILD_EXAMPLES}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  System: ${CMAKE_SYSTEM_NAME}")
message(STATUS "")
