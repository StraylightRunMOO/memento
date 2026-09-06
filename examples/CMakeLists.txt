cmake_minimum_required(VERSION 3.14)

# Simple C++ example
add_executable(example_simple example_simple.cpp)
target_include_directories(example_simple PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(example_simple PRIVATE Threads::Threads)

# C examples: leak hunting, worker pools, arenas, sized API
foreach(ex leak_hunt worker_pool arena sized)
    add_executable(example_${ex} example_${ex}.c)
    target_include_directories(example_${ex} PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(example_${ex} PRIVATE Threads::Threads)
endforeach()

if(MSVC)
    target_compile_definitions(example_simple PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

install(TARGETS example_simple example_leak_hunt example_worker_pool
                example_arena example_sized DESTINATION bin/examples)
