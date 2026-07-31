cmake_minimum_required(VERSION 3.14)

# Simple C++ example
add_executable(example_simple example_simple.cpp)
target_include_directories(example_simple PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(example_simple PRIVATE Threads::Threads)

if(MSVC)
    target_compile_definitions(example_simple PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

install(TARGETS example_simple DESTINATION bin/examples)
