cmake_minimum_required(VERSION 3.14)

# C tests
add_executable(test_core test_core.c)
target_include_directories(test_core PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_core PRIVATE pthread)
add_test(NAME core COMMAND test_core)

add_executable(test_thread test_thread.c)
target_include_directories(test_thread PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_thread PRIVATE pthread)
add_test(NAME thread COMMAND test_thread)

# C++ tests
add_executable(test_cpp test_cpp.cpp)
target_include_directories(test_cpp PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_cpp PRIVATE pthread)
set_property(TARGET test_cpp PROPERTY CXX_STANDARD 17)
add_test(NAME cpp COMMAND test_cpp)

# Set test properties
set_tests_properties(core thread cpp PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "MEMENTO_TEST=1"
)
