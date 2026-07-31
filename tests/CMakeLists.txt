cmake_minimum_required(VERSION 3.14)

# C tests
add_executable(test_core test_core.c)
target_include_directories(test_core PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_core PRIVATE Threads::Threads)
add_test(NAME core COMMAND test_core)

add_executable(test_thread test_thread.c)
target_include_directories(test_thread PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_thread PRIVATE Threads::Threads)
add_test(NAME thread COMMAND test_thread)

# C++ tests
add_executable(test_cpp test_cpp.cpp)
target_include_directories(test_cpp PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_cpp PRIVATE Threads::Threads)
set_property(TARGET test_cpp PROPERTY CXX_STANDARD 17)
add_test(NAME cpp COMMAND test_cpp)

# MSVC: enable C11 atomics path is N/A (Interlocked); silence CRT secure warnings
if(MSVC)
    target_compile_definitions(test_core PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(test_thread PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(test_cpp PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

set_tests_properties(core thread cpp PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "MEMENTO_TEST=1"
)
