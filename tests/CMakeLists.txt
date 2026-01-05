# Tests CMakeLists.txt

# C Tests
add_executable(test_memento_c test_memento.c)
target_link_libraries(test_memento_c m)
add_test(NAME test_memento_c COMMAND test_memento_c)

# C++ Tests
add_executable(test_memento_cpp test_memento_cpp.cpp)
target_link_libraries(test_memento_cpp m)
add_test(NAME test_memento_cpp COMMAND test_memento_cpp)

# Additional unit tests
add_executable(test_basic test_basic.c)
target_link_libraries(test_basic m)
add_test(NAME test_basic COMMAND test_basic)

add_executable(test_alignment test_alignment.c)
target_link_libraries(test_alignment m)
add_test(NAME test_alignment COMMAND test_alignment)

add_executable(test_stress test_stress.c)
target_link_libraries(test_stress m)
add_test(NAME test_stress COMMAND test_stress)

# Memory leak detection tests
add_executable(test_leaks test_leaks.c)
target_link_libraries(test_leaks m)
add_test(NAME test_leaks COMMAND test_leaks)

# Set test properties
set_tests_properties(
    test_memento_c test_memento_cpp test_basic test_alignment 
    test_stress test_leaks
    PROPERTIES
    TIMEOUT 60
    FAIL_REGULAR_EXPRESSION "FAILED|failed|error|Error"
)