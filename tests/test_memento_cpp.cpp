/*
 * Memento Allocator C++ Wrapper Test Suite
 * 
 * Tests for the C++17 wrapper of the memento memory allocator library.
 */

#include <iostream>
#include <memory>
#include <vector>
#include <string>
#include <cassert>
#include <chrono>
#include <random>

#define MEMENTO_IMPLEMENTATION
#include "memento.h"
#include "memento_cpp.hpp"

/* Test counters */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

/* Test macros */
#define TEST(name) void test_##name(); \
                   void test_##name()

#define RUN_TEST(name) do { \
    std::cout << "Running test: " << #name << "... " << std::flush; \
    tests_run++; \
    try { \
        test_##name(); \
        tests_passed++; \
        std::cout << "PASSED" << std::endl; \
    } catch (const std::exception& e) { \
        std::cout << "FAILED" << std::endl; \
        std::cout << "  Exception: " << e.what() << std::endl; \
        tests_failed++; \
    } catch (...) { \
        std::cout << "FAILED" << std::endl; \
        std::cout << "  Unknown exception" << std::endl; \
        tests_failed++; \
    } \
} while(0)

#define ASSERT(condition) do { \
    if (!(condition)) { \
        throw std::runtime_error("Assertion failed: " #condition); \
    } \
} while(0)

#define ASSERT_EQ(expected, actual) do { \
    if ((expected) != (actual)) { \
        throw std::runtime_error("Expected: " + std::to_string(expected) + ", Actual: " + std::to_string(actual)); \
    } \
} while(0)

#define ASSERT_NEQ(expected, actual) do { \
    if ((expected) == (actual)) { \
        throw std::runtime_error("Values should not be equal: " + std::to_string(actual)); \
    } \
} while(0)

#define ASSERT_NULL(ptr) ASSERT((ptr) == nullptr)
#define ASSERT_NOT_NULL(ptr) ASSERT((ptr) != nullptr)

/* ============================================================================= */
/* BASIC FUNCTIONALITY TESTS                                                   */
/* ============================================================================= */

TEST(initialization) {
    /* Simplified initialization test - just verify the library is working */
    
    /* Ensure library is initialized (it should be from the test setup) */
    if (!memento::is_initialized()) {
        memento::initialize();
    }
    ASSERT(memento::is_initialized());
    
    /* Test RAII initialization guard */
    {
        memento::scoped_init init;
        ASSERT(memento::is_initialized());
    }
    
    /* Should still be initialized */
    ASSERT(memento::is_initialized());
}

TEST(allocator_creation) {
    memento::scoped_init init;
    
    /* Test different allocator types */
    auto thread_cache = memento::allocator::create_thread_cache("test_thread");
    ASSERT_NOT_NULL(thread_cache.get_internal_allocator());
    ASSERT(std::string(thread_cache.name()) == "test_thread");
    
    auto block = memento::allocator::create_block("test_block", &thread_cache);
    ASSERT_NOT_NULL(block.get_internal_allocator());
    ASSERT(std::string(block.name()) == "test_block");
    
    auto proxy = memento::allocator::create_proxy("test_proxy", &thread_cache);
    ASSERT_NOT_NULL(proxy.get_internal_allocator());
    ASSERT(std::string(proxy.name()) == "test_proxy");
    
    auto stack = memento::allocator::create_stack("test_stack", 64 * 1024, &thread_cache);
    ASSERT_NOT_NULL(stack.get_internal_allocator());
    ASSERT(std::string(stack.name()) == "test_stack");
}

TEST(basic_allocation) {
    memento::scoped_init init;
    auto allocator = memento::allocator::create_thread_cache("basic_test");
    
    /* Test basic allocation */
    void* ptr = allocator.allocate(1024);
    ASSERT_NOT_NULL(ptr);
    
    /* Test deallocation */
    allocator.deallocate(ptr);
    
    /* Test zero-size allocation */
    void* zero_ptr = allocator.allocate(0);
    ASSERT_NULL(zero_ptr);
}

TEST(aligned_allocation) {
    memento::scoped_init init;
    auto allocator = memento::allocator::create_thread_cache("aligned_test");
    
    /* Test aligned allocation */
    void* ptr = allocator.allocate(1024, 64);
    ASSERT_NOT_NULL(ptr);
    
    /* Verify alignment */
    uintptr_t ptr_val = reinterpret_cast<uintptr_t>(ptr);
    ASSERT_EQ(0, ptr_val % 64);
    
    allocator.deallocate(ptr);
}

TEST(template_allocation) {
    memento::scoped_init init;
    auto allocator = memento::allocator::create_thread_cache("template_test");
    
    /* Test typed allocation */
    int* int_ptr = allocator.allocate_object<int>();
    ASSERT_NOT_NULL(int_ptr);
    
    *int_ptr = 42;
    ASSERT_EQ(42, *int_ptr);
    
    allocator.deallocate(int_ptr);
    
    /* Test array allocation */
    double* double_array = allocator.allocate_object<double>(10);
    ASSERT_NOT_NULL(double_array);
    
    for (int i = 0; i < 10; i++) {
        double_array[i] = static_cast<double>(i);
    }
    
    allocator.deallocate(double_array);
}

TEST(object_construction) {
    memento::scoped_init init;
    auto allocator = memento::allocator::create_thread_cache("construction_test");
    
    /* Test object construction */
    struct TestObject {
        int value;
        double data;
        TestObject(int v, double d) : value(v), data(d) {}
    };
    
    TestObject* obj = allocator.construct<TestObject>(42, 3.14);
    ASSERT_NOT_NULL(obj);
    ASSERT_EQ(42, obj->value);
    ASSERT_EQ(3.14, obj->data);
    
    /* Test destruction */
    allocator.destroy(obj);
}

/* ============================================================================= */
/* ARENA TESTS                                                                 */
/* ============================================================================= */

TEST(arena_basic) {
    memento::scoped_init init;
    auto backing_allocator = memento::allocator::create_thread_cache("arena_backing");
    memento::arena<int> int_arena("int_arena", &backing_allocator);
    
    ASSERT(std::string(int_arena.name()) == "int_arena");
    
    /* Test arena allocation */
    int* ptr = int_arena.allocate();
    ASSERT_NOT_NULL(ptr);
    
    *ptr = 123;
    ASSERT_EQ(123, *ptr);
    
    int_arena.deallocate(ptr);
}

TEST(arena_construction) {
    memento::scoped_init init;
    auto backing_allocator = memento::allocator::create_thread_cache("arena_const_backing");
    
    struct ComplexObject {
        std::string name;
        int value;
        double data;
        
        ComplexObject(const std::string& n, int v, double d) 
            : name(n), value(v), data(d) {}
    };
    
    memento::arena<ComplexObject> obj_arena("complex_arena", &backing_allocator);
    
    ComplexObject* obj = obj_arena.make("TestObject", 42, 2.718);
    ASSERT_NOT_NULL(obj);
    ASSERT(obj->name == "TestObject");
    ASSERT_EQ(42, obj->value);
    ASSERT_EQ(2.718, obj->data);
    
    obj_arena.destroy(obj);
}

/* ============================================================================= */
/* UNIQUE_PTR TESTS                                                            */
/* ============================================================================= */

TEST(unique_ptr_basic) {
    memento::scoped_init init;
    auto allocator = memento::allocator::create_thread_cache("unique_ptr_test");
    
    /* Test basic unique_ptr functionality */
    {
        int* raw_ptr = allocator.allocate_object<int>();
        *raw_ptr = 42;
        
        memento::unique_ptr<int> uptr(raw_ptr, &allocator);
        ASSERT_NOT_NULL(uptr.get());
        ASSERT_EQ(42, *uptr);
        ASSERT(uptr);
        
        /* Should be automatically destroyed when going out of scope */
    }
    
    /* Test move semantics */
    {
        int* raw_ptr = allocator.allocate_object<int>();
        *raw_ptr = 123;
        
        memento::unique_ptr<int> uptr1(raw_ptr, &allocator);
        memento::unique_ptr<int> uptr2(std::move(uptr1));
        
        ASSERT_NULL(uptr1.get());
        ASSERT_NOT_NULL(uptr2.get());
        ASSERT_EQ(123, *uptr2);
        
        /* Reset with new value */
        int* new_ptr = allocator.allocate_object<int>();
        *new_ptr = 456;
        uptr2.reset(new_ptr);
        
        ASSERT_EQ(456, *uptr2);
    }
}

/* ============================================================================= */
/* STL ALLOCATOR TESTS                                                         */
/* ============================================================================= */

TEST(stl_allocator_basic) {
    memento::scoped_init init;
    auto memento_allocator = memento::allocator::create_thread_cache("stl_test");
    
    /* Test STL-compatible allocator */
    memento::stl_allocator<int> stl_alloc(&memento_allocator);
    
    /* Allocate and deallocate */
    int* ptr = stl_alloc.allocate(10);
    ASSERT_NOT_NULL(ptr);
    
    for (int i = 0; i < 10; i++) {
        stl_alloc.construct(ptr + i, i);
    }
    
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(i, ptr[i]);
        stl_alloc.destroy(ptr + i);
    }
    
    stl_alloc.deallocate(ptr, 10);
}

TEST(stl_allocator_vector) {
    memento::scoped_init init;
    auto memento_allocator = memento::allocator::create_thread_cache("stl_vector_test");
    
    /* Test with std::vector */
    using int_allocator = memento::stl_allocator<int>;
    using int_vector = std::vector<int, int_allocator>;
    
    int_allocator alloc(&memento_allocator);
    int_vector vec(alloc);
    
    /* Add elements */
    for (int i = 0; i < 100; i++) {
        vec.push_back(i);
    }
    
    ASSERT_EQ(100, vec.size());
    
    for (int i = 0; i < 100; i++) {
        ASSERT_EQ(i, vec[i]);
    }
    
    /* Test clear */
    vec.clear();
    ASSERT_EQ(0, vec.size());
}

/* ============================================================================= */
/* STATISTICS TESTS                                                            */
/* ============================================================================= */

TEST(statistics_tracking) {
    memento::scoped_init init;
    auto allocator = memento::allocator::create_thread_cache("stats_test");
    
    auto initial_stats = allocator.stats();
    size_t initial_alloc_count = initial_stats.allocation_count;
    
    /* Perform allocations */
    void* ptr1 = allocator.allocate(1024);
    void* ptr2 = allocator.allocate(2048);
    void* ptr3 = allocator.allocate(4096);
    
    ASSERT_NOT_NULL(ptr1);
    ASSERT_NOT_NULL(ptr2);
    ASSERT_NOT_NULL(ptr3);
    
    /* Check statistics */
    auto after_alloc_stats = allocator.stats();
    ASSERT_EQ(initial_alloc_count + 3, after_alloc_stats.allocation_count);
    ASSERT(after_alloc_stats.current_usage > 0);
    
    /* Deallocate */
    allocator.deallocate(ptr1);
    allocator.deallocate(ptr2);
    allocator.deallocate(ptr3);
    
    /* Check final statistics */
    auto final_stats = allocator.stats();
    ASSERT_EQ(initial_alloc_count + 3, final_stats.allocation_count);
    ASSERT_EQ(initial_alloc_count + 3, final_stats.deallocation_count);
}

/* ============================================================================= */
/* ERROR HANDLING TESTS                                                        */
/* ============================================================================= */

TEST(error_handling) {
    memento::scoped_init init;
    auto allocator = memento::allocator::create_thread_cache("error_test");
    
    /* Test null allocator in constructor */
    try {
        memento::allocator bad_alloc(nullptr);
        ASSERT(false);  /* Should not reach here */
    } catch (const std::invalid_argument&) {
        /* Expected */
    }
    
    /* Test invalid alignment */
    try {
        static_cast<void>(allocator.allocate(1024, 3));  /* Not power of two */
        ASSERT(false);  /* Should not reach here */
    } catch (const std::invalid_argument&) {
        /* Expected */
    }
    
    /* Test excessive alignment */
    try {
        static_cast<void>(allocator.allocate(1024, MEMENTO_MAX_ALIGNMENT * 2));
        ASSERT(false);  /* Should not reach here */
    } catch (const std::invalid_argument&) {
        /* Expected */
    }
    
    /* Test allocation error */
    try {
        /* Try to allocate an enormous amount */
        static_cast<void>(allocator.allocate(SIZE_MAX));
        ASSERT(false);  /* Should not reach here */
    } catch (const memento::allocation_error& e) {
        /* Expected */
        ASSERT(e.requested_size() == SIZE_MAX);
    }
}

/* ============================================================================= */
/* PERFORMANCE TESTS                                                           */
/* ============================================================================= */

TEST(performance_comparison) {
    memento::scoped_init init;
    
    auto thread_cache = memento::allocator::create_thread_cache("perf_thread");
    auto block = memento::allocator::create_block("perf_block", &thread_cache);
    
    const int iterations = 100000;
    
    /* Test thread cache performance */
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < iterations; i++) {
        void* ptr = thread_cache.allocate(64);
        thread_cache.deallocate(ptr);
    }
    
    auto thread_cache_time = std::chrono::high_resolution_clock::now() - start;
    
    /* Test block allocator performance */
    start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < iterations; i++) {
        void* ptr = block.allocate(64);
        block.deallocate(ptr);
    }
    
    auto block_time = std::chrono::high_resolution_clock::now() - start;
    
    /* Print performance comparison */
    auto thread_cache_us = std::chrono::duration_cast<std::chrono::microseconds>(thread_cache_time).count();
    auto block_us = std::chrono::duration_cast<std::chrono::microseconds>(block_time).count();
    
    std::cout << "\n  Performance comparison (" << iterations << " allocations):" << std::endl;
    std::cout << "    Thread cache: " << thread_cache_us << " μs" << std::endl;
    std::cout << "    Block: " << block_us << " μs" << std::endl;
    std::cout << "    Per allocation:" << std::endl;
    std::cout << "      Thread cache: " << (thread_cache_us / (double)iterations) << " μs" << std::endl;
    std::cout << "      Block: " << (block_us / (double)iterations) << " μs" << std::endl;
}

/* ============================================================================= */
/* COMPLEX SCENARIO TESTS                                                      */
/* ============================================================================= */

TEST(complex_allocator_hierarchy) {
    memento::scoped_init init;
    
    /* Create complex allocator hierarchy */
    auto root = memento::allocator::create_thread_cache("root");
    auto system_proxy = memento::allocator::create_proxy("system", &root);
    auto graphics_proxy = memento::allocator::create_proxy("graphics", &root);
    auto audio_proxy = memento::allocator::create_proxy("audio", &root);
    
    auto graphics_block = memento::allocator::create_block("graphics_block", &graphics_proxy);
    auto audio_block = memento::allocator::create_block("audio_block", &audio_proxy);
    
    /* Test allocations in different subsystems */
    struct Vertex { float x, y, z; };
    struct AudioSample { float left, right; };
    
    memento::arena<Vertex> vertex_arena("vertices", &graphics_block);
    memento::arena<AudioSample> sample_arena("samples", &audio_block);
    
    /* Allocate some objects */
    Vertex* vertices = vertex_arena.allocate(1000);
    AudioSample* samples = sample_arena.allocate(500);
    
    ASSERT_NOT_NULL(vertices);
    ASSERT_NOT_NULL(samples);
    
    /* Initialize data */
    for (int i = 0; i < 1000; i++) {
        vertices[i].x = static_cast<float>(i);
        vertices[i].y = static_cast<float>(i * 2);
        vertices[i].z = static_cast<float>(i * 3);
    }
    
    for (int i = 0; i < 500; i++) {
        samples[i].left = static_cast<float>(i) / 500.0f;
        samples[i].right = static_cast<float>(500 - i) / 500.0f;
    }
    
    /* Verify data */
    ASSERT_EQ(0.0f, vertices[0].x);
    ASSERT_EQ(999.0f, vertices[999].x);
    ASSERT_EQ(0.0f, samples[0].left);
    ASSERT_EQ(1.0f, samples[0].right);
    
    /* Deallocate */
    vertex_arena.deallocate(vertices);
    sample_arena.deallocate(samples);
    
    /* Print statistics for all allocators */
    std::cout << "\n  Hierarchy statistics:" << std::endl;
    root.print_stats();
    system_proxy.print_stats();
    graphics_proxy.print_stats();
    audio_proxy.print_stats();
}

/* ============================================================================= */
/* UTILITY FUNCTIONS                                                           */
/* ============================================================================= */

static void print_test_summary(void) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test Summary:" << std::endl;
    std::cout << "  Total tests run: " << tests_run << std::endl;
    std::cout << "  Tests passed: " << tests_passed << std::endl;
    std::cout << "  Tests failed: " << tests_failed << std::endl;
    std::cout << "========================================" << std::endl;
}

static int run_all_tests(void) {
    std::cout << "Memento C++ Wrapper Test Suite" << std::endl;
    std::cout << "==============================" << std::endl << std::endl;
    
    try {
        /* Basic functionality tests */
        std::cout << "=== Basic Functionality Tests ===" << std::endl;
        RUN_TEST(initialization);
        RUN_TEST(allocator_creation);
        RUN_TEST(basic_allocation);
        RUN_TEST(aligned_allocation);
        RUN_TEST(template_allocation);
        RUN_TEST(object_construction);
        
        /* Arena tests */
        std::cout << "\n=== Arena Tests ===" << std::endl;
        RUN_TEST(arena_basic);
        RUN_TEST(arena_construction);
        
        /* Unique pointer tests */
        std::cout << "\n=== Unique Pointer Tests ===" << std::endl;
        RUN_TEST(unique_ptr_basic);
        
        /* STL allocator tests */
        std::cout << "\n=== STL Allocator Tests ===" << std::endl;
        RUN_TEST(stl_allocator_basic);
        RUN_TEST(stl_allocator_vector);
        
        /* Statistics tests */
        std::cout << "\n=== Statistics Tests ===" << std::endl;
        RUN_TEST(statistics_tracking);
        
        /* Error handling tests */
        std::cout << "\n=== Error Handling Tests ===" << std::endl;
        RUN_TEST(error_handling);
        
        /* Performance tests */
        std::cout << "\n=== Performance Tests ===" << std::endl;
        RUN_TEST(performance_comparison);
        
        /* Complex scenario tests */
        std::cout << "\n=== Complex Scenario Tests ===" << std::endl;
        RUN_TEST(complex_allocator_hierarchy);
        
    } catch (const std::exception& e) {
        std::cout << "Test suite failed with exception: " << e.what() << std::endl;
        return 1;
    }
    
    /* Print summary */
    print_test_summary();
    
    return tests_failed > 0 ? 1 : 0;
}

/* ============================================================================= */
/* MAIN FUNCTION                                                               */
/* ============================================================================= */

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    
    try {
        int result = run_all_tests();
        
        if (result == 0) {
            std::cout << "\nAll tests passed! ✓" << std::endl;
        } else {
            std::cout << "\nSome tests failed! ✗" << std::endl;
        }
        
        return result;
    } catch (const std::exception& e) {
        std::cout << "Fatal error in test suite: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cout << "Fatal unknown error in test suite" << std::endl;
        return 1;
    }
}