cmake_minimum_required(VERSION 3.14)
project(Memento VERSION 1.0.0 LANGUAGES C CXX)

# C and C++ standards
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build options
option(MEMENTO_BUILD_TESTS "Build test suite" ON)
option(MEMENTO_BUILD_BENCHMARKS "Build benchmarks" ON)
option(MEMENTO_BUILD_EXAMPLES "Build examples" ON)
option(MEMENTO_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MEMENTO_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(MEMENTO_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

# Compiler flags
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(MEMENTO_C_FLAGS "-Wall -Wextra -Wpedantic -Werror")
    set(MEMENTO_CXX_FLAGS "-Wall -Wextra -Wpedantic -Werror")
    
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(MEMENTO_C_FLAGS "${MEMENTO_C_FLAGS} -g -O0 -DMEMENTO_ENABLE_DEBUG_CHECKS=1")
        set(MEMENTO_CXX_FLAGS "${MEMENTO_CXX_FLAGS} -g -O0 -DMEMENTO_ENABLE_DEBUG_CHECKS=1")
    else()
        set(MEMENTO_C_FLAGS "${MEMENTO_C_FLAGS} -O3 -DNDEBUG")
        set(MEMENTO_CXX_FLAGS "${MEMENTO_CXX_FLAGS} -O3 -DNDEBUG")
    endif()
    
    # Sanitizer support
    if(MEMENTO_ENABLE_ASAN)
        set(MEMENTO_C_FLAGS "${MEMENTO_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
        set(MEMENTO_CXX_FLAGS "${MEMENTO_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
        set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fsanitize=address")
    endif()
    
    if(MEMENTO_ENABLE_UBSAN)
        set(MEMENTO_C_FLAGS "${MEMENTO_C_FLAGS} -fsanitize=undefined")
        set(MEMENTO_CXX_FLAGS "${MEMENTO_CXX_FLAGS} -fsanitize=undefined")
        set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fsanitize=undefined")
    endif()
    
    if(MEMENTO_ENABLE_TSAN)
        set(MEMENTO_C_FLAGS "${MEMENTO_C_FLAGS} -fsanitize=thread")
        set(MEMENTO_CXX_FLAGS "${MEMENTO_CXX_FLAGS} -fsanitize=thread")
        set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fsanitize=thread")
    endif()
elseif(MSVC)
    set(MEMENTO_C_FLAGS "/W4 /WX")
    set(MEMENTO_CXX_FLAGS "/W4 /WX")
    
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(MEMENTO_C_FLAGS "${MEMENTO_C_FLAGS} /Od /DMEMENTO_ENABLE_DEBUG_CHECKS=1")
        set(MEMENTO_CXX_FLAGS "${MEMENTO_CXX_FLAGS} /Od /DMEMENTO_ENABLE_DEBUG_CHECKS=1")
    else()
        set(MEMENTO_C_FLAGS "${MEMENTO_C_FLAGS} /O2 /DNDEBUG")
        set(MEMENTO_CXX_FLAGS "${MEMENTO_CXX_FLAGS} /O2 /DNDEBUG")
    endif()
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${MEMENTO_C_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${MEMENTO_CXX_FLAGS}")

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Enable testing
enable_testing()

# Add subdirectories
if(MEMENTO_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(MEMENTO_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()

if(MEMENTO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Installation (simplified for header-only library)
install(FILES 
    memento.h 
    memento_cpp.hpp 
    README.md 
    DESTINATION include/memento
)

# Summary
message(STATUS "Memento Configuration:")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Build Tests: ${MEMENTO_BUILD_TESTS}")
message(STATUS "  Build Benchmarks: ${MEMENTO_BUILD_BENCHMARKS}")
message(STATUS "  Build Examples: ${MEMENTO_BUILD_EXAMPLES}")
message(STATUS "  AddressSanitizer: ${MEMENTO_ENABLE_ASAN}")
message(STATUS "  UndefinedBehaviorSanitizer: ${MEMENTO_ENABLE_UBSAN}")
message(STATUS "  ThreadSanitizer: ${MEMENTO_ENABLE_TSAN}")