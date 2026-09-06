cmake_minimum_required(VERSION 3.14)

# Optional coverage build: cmake -DMEMENTO_COVERAGE=ON
if(MEMENTO_COVERAGE AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

# C tests
add_executable(test_core test_core.c)
target_include_directories(test_core PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_core PRIVATE Threads::Threads)
add_test(NAME core COMMAND test_core)

add_executable(test_thread test_thread.c)
target_include_directories(test_thread PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_thread PRIVATE Threads::Threads)
add_test(NAME thread COMMAND test_thread)

add_executable(test_sized test_sized.c)
target_include_directories(test_sized PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_sized PRIVATE Threads::Threads)
add_test(NAME sized COMMAND test_sized)

# The MEMENTO_SIZED=1 build mode: thread-heap API carries headers
add_executable(test_sized_mode test_sized_mode.c)
target_include_directories(test_sized_mode PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_sized_mode PRIVATE MEMENTO_SIZED=1)
target_link_libraries(test_sized_mode PRIVATE Threads::Threads)
add_test(NAME sized_mode COMMAND test_sized_mode)

# Instrumented proxies
add_executable(test_proxy test_proxy.c)
target_include_directories(test_proxy PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_proxy PRIVATE MEMENTO_PROXY_DEBUG=1)
target_link_libraries(test_proxy PRIVATE Threads::Threads)
add_test(NAME proxy COMMAND test_proxy)

# Release proxies: same API, instrumentation compiled out
add_executable(test_proxy_release test_proxy.c)
target_include_directories(test_proxy_release PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_proxy_release PRIVATE MEMENTO_PROXY_DEBUG=0)
target_link_libraries(test_proxy_release PRIVATE Threads::Threads)
add_test(NAME proxy_release COMMAND test_proxy_release)

# fork / thread-exit / park-adopt lifecycle
add_executable(test_lifecycle test_lifecycle.c)
target_include_directories(test_lifecycle PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_lifecycle PRIVATE Threads::Threads)
add_test(NAME lifecycle COMMAND test_lifecycle)

# Debug mode: canaries + double-free tripwires (+ ASan poison when built with it)
add_executable(test_debug test_debug.c)
target_include_directories(test_debug PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_debug PRIVATE MEMENTO_DEBUG=1)
target_link_libraries(test_debug PRIVATE Threads::Threads)
add_test(NAME debug COMMAND test_debug)

# Stats-free build: MEMENTO_STATS=0 must compile and behave
add_executable(test_nostats test_core.c)
target_include_directories(test_nostats PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_nostats PRIVATE MEMENTO_STATS=0)
target_link_libraries(test_nostats PRIVATE Threads::Threads)
add_test(NAME nostats COMMAND test_nostats)

# C++ tests
add_executable(test_cpp test_cpp.cpp)
target_include_directories(test_cpp PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_cpp PRIVATE Threads::Threads)
set_property(TARGET test_cpp PROPERTY CXX_STANDARD 17)
add_test(NAME cpp COMMAND test_cpp)

# MSVC: silence CRT secure warnings
if(MSVC)
    foreach(t test_core test_thread test_sized test_sized_mode test_nostats test_proxy
               test_proxy_release test_lifecycle test_debug test_cpp)
        target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endforeach()
endif()

set_tests_properties(core thread sized sized_mode proxy proxy_release
                     lifecycle debug cpp PROPERTIES
    TIMEOUT 120
    ENVIRONMENT "MEMENTO_TEST=1"
)

# ---------------------------------------------------------------------------
# malloc interposition shim: build the LD_PRELOAD library and run a plain
# libc-malloc consumer under it (Linux/macOS only)
# ---------------------------------------------------------------------------
if(UNIX)
    add_library(memento_preload SHARED ${CMAKE_SOURCE_DIR}/shim/memento_malloc.c)
    target_include_directories(memento_preload PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(memento_preload PRIVATE Threads::Threads)

    add_executable(shimcheck shimcheck.c)

    if(APPLE)
        set(MEMENTO_PRELOAD_VAR "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:memento_preload>;DYLD_FORCE_FLAT_NAMESPACE=1")
    else()
        set(MEMENTO_PRELOAD_VAR "LD_PRELOAD=$<TARGET_FILE:memento_preload>")
    endif()

    add_test(NAME shim COMMAND ${CMAKE_COMMAND} -E env
        "${MEMENTO_PRELOAD_VAR}" $<TARGET_FILE:shimcheck>)
    set_tests_properties(shim PROPERTIES TIMEOUT 60)

    # Same, with the atexit heap dump enabled (exercises the report path)
    add_test(NAME shim_dump COMMAND ${CMAKE_COMMAND} -E env
        "${MEMENTO_PRELOAD_VAR}" "MEMENTO_DUMP_ATEXIT=1" $<TARGET_FILE:shimcheck>)
    set_tests_properties(shim_dump PROPERTIES TIMEOUT 60)
endif()
