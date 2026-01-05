# Examples CMakeLists.txt

# Basic examples
add_executable(example_basic example_basic.c)
target_link_libraries(example_basic m)

add_executable(example_thread_cache example_thread_cache.c)
target_link_libraries(example_thread_cache m)

add_executable(example_block_allocator example_block_allocator.c)
target_link_libraries(example_block_allocator m)

add_executable(example_proxy_allocator example_proxy_allocator.c)
target_link_libraries(example_proxy_allocator m)

add_executable(example_stack_allocator example_stack_allocator.c)
target_link_libraries(example_stack_allocator m)

# Advanced examples
add_executable(example_hierarchy example_hierarchy.c)
target_link_libraries(example_hierarchy m)

add_executable(example_statistics example_statistics.c)
target_link_libraries(example_statistics m)

add_executable(example_alignment example_alignment.c)
target_link_libraries(example_alignment m)

# C++ examples
add_executable(example_cpp_basic example_cpp_basic.cpp)
target_link_libraries(example_cpp_basic m)

add_executable(example_cpp_arena example_cpp_arena.cpp)
target_link_libraries(example_cpp_arena m)

add_executable(example_cpp_stl example_cpp_stl.cpp)
target_link_libraries(example_cpp_stl m)

add_executable(example_cpp_custom example_cpp_custom.cpp)
target_link_libraries(example_cpp_custom m)

# Game engine example
add_executable(example_game_engine example_game_engine.cpp)
target_link_libraries(example_game_engine m)

# Set properties
set_target_properties(
    example_basic example_thread_cache example_block_allocator
    example_proxy_allocator example_stack_allocator example_hierarchy
    example_statistics example_alignment example_cpp_basic
    example_cpp_arena example_cpp_stl example_cpp_custom
    example_game_engine
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
)