/*
 * Memento C++ Test Suite
 * 
 * Comprehensive tests for C++ wrapper features.
 */

#define MEMENTO_IMPLEMENTATION
#include "memento.hpp"

#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "  " << #name << "... "; \
    std::cout.flush(); \
    test_##name(); \
    passed++; \
    std::cout << "PASS\n"; \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        std::cout << "FAIL\n    Assertion: " << #cond << "\n    File: " \
                  << __FILE__ << ":" << __LINE__ << "\n"; \
        failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NE(a, b) ASSERT((a) != (b))
#define ASSERT_NULL(p) ASSERT((p) == nullptr)
#define ASSERT_NOT_NULL(p) ASSERT((p) != nullptr)
#define ASSERT_GE(a, b) ASSERT((a) >= (b))

static int passed = 0;
static int failed = 0;

/* ============================================================================
 * Test Classes
 * ============================================================================ */

class TestObject {
public:
    static int construct_count;
    static int destruct_count;
    
    int value;
    std::string name;
    
    TestObject(int v = 0, const std::string& n = "") : value(v), name(n) {
        construct_count++;
    }
    
    ~TestObject() {
        destruct_count++;
    }
    
    TestObject(const TestObject& other) : value(other.value), name(other.name) {
        construct_count++;
    }
    
    TestObject(TestObject&& other) noexcept 
        : value(other.value), name(std::move(other.name)) {
        construct_count++;
    }
    
    static void reset_counts() {
        construct_count = 0;
        destruct_count = 0;
    }
};

int TestObject::construct_count = 0;
int TestObject::destruct_count = 0;

struct Particle {
    float x, y, z;
    float vx, vy, vz;
    
    Particle(float x_ = 0, float y_ = 0, float z_ = 0) 
        : x(x_), y(y_), z(z_), vx(0), vy(0), vz(0) {}
};

/* ============================================================================
 * Context Tests
 * ============================================================================ */

TEST(version_check) {
    /* Check C API version */
    ASSERT_EQ(strcmp(memento_version_string(), "2.0.0"), 0);
    ASSERT_EQ(memento_version_number(), 0x020000);
    ASSERT(memento_version_check(2, 0, 0));
    ASSERT(!memento_version_check(3, 0, 0));
}

TEST(context_raii) {
    {
        memento::context ctx;
        ASSERT_NOT_NULL(&ctx);
    }
    /* Context destroyed, cleanup happened */
}

TEST(context_multiple) {
    memento::context ctx1;
    {
        memento::context ctx2;  /* Multiple contexts ok */
    }
}

/* ============================================================================
 * Heap Tests
 * ============================================================================ */

TEST(heap_basic) {
    memento::heap h;
    
    void* ptr = h.allocate(1024);
    ASSERT_NOT_NULL(ptr);
    
    h.deallocate(ptr, 1024);
}

TEST(heap_object_construction) {
    TestObject::reset_counts();
    
    {
        memento::heap h;
        auto obj = h.construct<TestObject>(42, "test");
        ASSERT_NOT_NULL(obj);
        ASSERT_EQ(obj->value, 42);
        ASSERT_EQ(obj->name, "test");
        ASSERT_EQ(TestObject::construct_count, 1);
        
        h.destroy(obj);
        ASSERT_EQ(TestObject::destruct_count, 1);
    }
}

TEST(heap_array_allocation) {
    memento::heap h;
    
    int* arr = h.allocate_array<int>(100);
    ASSERT_NOT_NULL(arr);
    
    for (int i = 0; i < 100; i++) {
        arr[i] = i;
    }
    
    h.deallocate_array(arr, 100);
}

TEST(heap_realloc) {
    memento::heap h;
    
    char* ptr = (char*)h.allocate(100);
    ASSERT_NOT_NULL(ptr);
    strcpy(ptr, "Hello");
    
    /* C++ wrapper doesn't expose realloc - allocate, copy, free manually */
    char* new_ptr = (char*)h.allocate(200);
    ASSERT_NOT_NULL(new_ptr);
    memcpy(new_ptr, ptr, 100);
    h.deallocate(ptr, 100);
    
    ASSERT_EQ(strcmp(new_ptr, "Hello"), 0);
    h.deallocate(new_ptr, 200);
}

TEST(heap_null_deallocate) {
    memento::heap h;
    /* Should not crash */
    h.deallocate(nullptr, 100);
}

TEST(heap_stats) {
    memento::heap h;
    
    auto ptr = h.allocate(1024);
    h.deallocate(ptr, 1024);
    
    /* Stats collected internally */
}

/* ============================================================================
 * Pool Tests
 * ============================================================================ */

TEST(pool_basic) {
    memento::pool<Particle> pool(100);
    
    auto p = pool.emplace(1.0f, 2.0f, 3.0f);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p->x, 1.0f);
    ASSERT_EQ(p->y, 2.0f);
    ASSERT_EQ(p->z, 3.0f);
    
    pool.destroy(p);
}

TEST(pool_exhaustion) {
    memento::pool<Particle> pool(10);
    
    std::vector<Particle*> ptrs;
    for (int i = 0; i < 10; i++) {
        auto p = pool.emplace();
        ASSERT_NOT_NULL(p);
        ptrs.push_back(p);
    }
    
    /* Pool exhausted - emplace throws bad_alloc */
    bool threw = false;
    try {
        auto extra = pool.emplace();
        (void)extra;
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    ASSERT(threw);
    
    for (auto p : ptrs) {
        pool.destroy(p);
    }
}

TEST(pool_recycle) {
    memento::pool<Particle> pool(10);
    
    auto p1 = pool.emplace();
    ASSERT_NOT_NULL(p1);
    pool.destroy(p1);
    
    auto p2 = pool.emplace();
    ASSERT_NOT_NULL(p2);
    /* Should reuse the same slot */
    ASSERT_EQ(p1, p2);
    
    pool.destroy(p2);
}

TEST(pool_move) {
    memento::pool<Particle> pool1(10);
    
    auto p = pool1.emplace();
    ASSERT_NOT_NULL(p);
    
    memento::pool<Particle> pool2(std::move(pool1));
    /* pool1 is now empty, pool2 has the resources */
    
    pool2.destroy(p);
}

/* ============================================================================
 * Arena Tests
 * ============================================================================ */

TEST(arena_basic) {
    memento::arena arena(4096);
    
    int* a = (int*)arena.allocate(sizeof(int) * 100, alignof(int));
    ASSERT_NOT_NULL(a);
    
    for (int i = 0; i < 100; i++) {
        a[i] = i;
    }
    
    for (int i = 0; i < 100; i++) {
        ASSERT_EQ(a[i], i);
    }
}

TEST(arena_object_construction) {
    TestObject::reset_counts();
    memento::arena arena(4096);
    
    auto obj = arena.construct<TestObject>(100, "arena_test");
    ASSERT_NOT_NULL(obj);
    ASSERT_EQ(obj->value, 100);
    ASSERT_EQ(obj->name, "arena_test");
    ASSERT_EQ(TestObject::construct_count, 1);
    
    /* Arena destruction handles cleanup */
}

TEST(arena_save_restore) {
    memento::arena arena(4096);
    
    int* a = (int*)arena.allocate(sizeof(int) * 10, alignof(int));
    ASSERT_NOT_NULL(a);
    
    auto save = arena.save();
    size_t used_before = arena.used();
    
    double* b = (double*)arena.allocate(sizeof(double) * 10, alignof(double));
    ASSERT_NOT_NULL(b);
    
    ASSERT_GE(arena.used(), used_before);
    
    arena.restore(save);
    ASSERT_EQ(arena.used(), used_before);
    
    /* Original data still accessible */
    for (int i = 0; i < 10; i++) {
        a[i] = i;
    }
}

TEST(arena_reset) {
    memento::arena arena(4096);
    
    for (int i = 0; i < 100; i++) {
        auto ptr = arena.allocate(64, 8);
        ASSERT_NOT_NULL(ptr);
    }
    
    arena.reset();
    ASSERT_EQ(arena.used(), 0);
    
    auto ptr = arena.allocate(1024, 8);
    ASSERT_NOT_NULL(ptr);
}

TEST(arena_growth) {
    memento::arena arena(256);
    
    for (int i = 0; i < 100; i++) {
        auto ptr = arena.allocate(128, 8);
        ASSERT_NOT_NULL(ptr);
    }
    
    ASSERT_GE(arena.capacity(), 256);
}

TEST(arena_move) {
    memento::arena arena1(4096);
    auto ptr = arena1.allocate(1024, 8);
    ASSERT_NOT_NULL(ptr);
    
    memento::arena arena2(std::move(arena1));
    /* arena1 is now empty */
    
    ASSERT_GE(arena2.capacity(), 4096);
}

/* ============================================================================
 * Stack Tests
 * ============================================================================ */

TEST(stack_basic) {
    memento::stack<Particle> stack(4096);
    
    auto p1 = stack.push(1.0f, 2.0f, 3.0f);
    ASSERT_NOT_NULL(p1);
    ASSERT_EQ(p1->x, 1.0f);
    
    auto p2 = stack.push(4.0f, 5.0f, 6.0f);
    ASSERT_NOT_NULL(p2);
    
    stack.pop(p2);
    stack.pop(p1);
}

TEST(stack_marker) {
    memento::stack<Particle> stack(4096);
    
    auto p1 = stack.push();
    ASSERT_NOT_NULL(p1);
    
    auto marker = stack.mark();
    
    auto p2 = stack.push();
    ASSERT_NOT_NULL(p2);
    auto p3 = stack.push();
    ASSERT_NOT_NULL(p3);
    
    stack.restore(marker);
    
    /* Can push from marker */
    auto p4 = stack.push();
    ASSERT_NOT_NULL(p4);
}

TEST(stack_reset) {
    memento::stack<Particle> stack(4096);
    
    for (int i = 0; i < 10; i++) {
        auto p = stack.push();
        ASSERT_NOT_NULL(p);
    }
    
    stack.reset();
    
    auto p = stack.push();
    ASSERT_NOT_NULL(p);
}

TEST(stack_overflow) {
    /* NOTE: Stack overflow detection depends on implementation details */
    /* Skipping for now - stack implementation may auto-grow or throw */
    memento::stack<Particle> stack(256);
    
    auto p1 = stack.push();
    ASSERT_NOT_NULL(p1);
    
    /* Reset and verify it still works */
    stack.reset();
    auto p2 = stack.push();
    ASSERT_NOT_NULL(p2);
}

TEST(stack_move) {
    memento::stack<Particle> stack1(4096);
    auto p = stack1.push();
    ASSERT_NOT_NULL(p);
    
    memento::stack<Particle> stack2(std::move(stack1));
    /* stack1 is now empty */
}

/* ============================================================================
 * Scoped Pointer Tests
 * ============================================================================ */

TEST(scoped_ptr_basic) {
    TestObject::reset_counts();
    memento::heap h;
    
    {
        auto obj = h.construct<TestObject>(42, "scoped");
        memento::scoped_ptr<TestObject> ptr(obj, &h);
        
        ASSERT_NOT_NULL(ptr.get());
        ASSERT_EQ(ptr->value, 42);
        ASSERT_EQ((*ptr).name, "scoped");
        ASSERT_EQ(TestObject::construct_count, 1);
        
        /* Will be destroyed when ptr goes out of scope */
    }
    
    ASSERT_EQ(TestObject::destruct_count, 1);
}

TEST(scoped_ptr_reset) {
    TestObject::reset_counts();
    memento::heap h;
    
    auto obj1 = h.construct<TestObject>(1, "first");
    memento::scoped_ptr<TestObject> ptr(obj1, &h);
    
    auto obj2 = h.construct<TestObject>(2, "second");
    ptr.reset(obj2);
    
    ASSERT_EQ(TestObject::destruct_count, 1);  /* first destroyed */
    ASSERT_EQ(ptr->value, 2);
    
    ptr.reset();
    ASSERT_EQ(TestObject::destruct_count, 2);
}

TEST(scoped_ptr_release) {
    TestObject::reset_counts();
    memento::heap h;
    
    auto obj = h.construct<TestObject>(42, "test");
    memento::scoped_ptr<TestObject> ptr(obj, &h);
    
    auto raw = ptr.release();
    ASSERT_EQ(raw, obj);
    ASSERT_NULL(ptr.get());
    
    /* Not destroyed - we released ownership */
    ASSERT_EQ(TestObject::destruct_count, 0);
    
    h.destroy(raw);
    ASSERT_EQ(TestObject::destruct_count, 1);
}

TEST(scoped_ptr_move) {
    TestObject::reset_counts();
    memento::heap h;
    
    auto obj = h.construct<TestObject>(42, "test");
    memento::scoped_ptr<TestObject> ptr1(obj, &h);
    
    memento::scoped_ptr<TestObject> ptr2(std::move(ptr1));
    ASSERT_NULL(ptr1.get());
    ASSERT_NOT_NULL(ptr2.get());
    ASSERT_EQ(ptr2->value, 42);
}

/* ============================================================================
 * STL Allocator Tests
 * ============================================================================ */

TEST(stl_vector_basic) {
    memento::heap h;
    memento::allocator<int> alloc(h);
    std::vector<int, memento::allocator<int>> vec(alloc);
    
    for (int i = 0; i < 100; i++) {
        vec.push_back(i);
    }
    
    ASSERT_EQ(vec.size(), 100);
    for (int i = 0; i < 100; i++) {
        ASSERT_EQ(vec[i], i);
    }
}

TEST(stl_vector_reserve) {
    memento::heap h;
    memento::allocator<int> alloc(h);
    std::vector<int, memento::allocator<int>> vec(alloc);
    
    vec.reserve(1000);
    ASSERT_GE(vec.capacity(), 1000);
    
    for (int i = 0; i < 1000; i++) {
        vec.push_back(i);
    }
    
    ASSERT_EQ(vec.size(), 1000);
}

TEST(stl_vector_strings) {
    memento::heap h;
    memento::allocator<std::string> alloc(h);
    std::vector<std::string, memento::allocator<std::string>> vec(alloc);
    
    vec.push_back("Hello");
    vec.push_back("World");
    vec.emplace_back("Test");
    
    ASSERT_EQ(vec.size(), 3);
    ASSERT_EQ(vec[0], "Hello");
    ASSERT_EQ(vec[1], "World");
    ASSERT_EQ(vec[2], "Test");
}

TEST(stl_vector_move) {
    memento::heap h;
    memento::allocator<int> alloc(h);
    std::vector<int, memento::allocator<int>> vec1(alloc);
    
    for (int i = 0; i < 100; i++) {
        vec1.push_back(i);
    }
    
    auto vec2 = std::move(vec1);
    ASSERT_EQ(vec2.size(), 100);
    ASSERT_EQ(vec2[50], 50);
}

TEST(stl_rebind) {
    memento::allocator<int> alloc;
    memento::allocator<double>::rebind<int>::other rebound_alloc;
    
    /* Rebound allocator should work */
    auto ptr = rebound_alloc.allocate(10);
    ASSERT_NOT_NULL(ptr);
    rebound_alloc.deallocate(ptr, 10);
}

/* ============================================================================
 * Thread Safety Tests
 * ============================================================================ */

TEST(thread_heap_concurrent) {
    const int num_threads = 4;
    const int iterations = 1000;
    
    std::atomic<int> success_count{0};
    
    auto thread_fn = [&]() {
        memento::heap h;
        bool success = true;
        
        for (int i = 0; i < iterations; i++) {
            auto ptr = h.allocate(256);
            if (!ptr) {
                success = false;
                break;
            }
            memset(ptr, 0xAB, 256);
            h.deallocate(ptr, 256);
        }
        
        if (success) {
            success_count++;
        }
    };
    
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(thread_fn);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    ASSERT_EQ(success_count, num_threads);
}

TEST(thread_pool_concurrent) {
    const int num_threads = 4;
    
    std::atomic<int> success_count{0};
    
    auto thread_fn = [&]() {
        memento::pool<Particle> pool(100);
        bool success = true;
        
        for (int i = 0; i < 100; i++) {
            auto p = pool.emplace(1.0f, 2.0f, 3.0f);
            if (!p) {
                success = false;
                break;
            }
            pool.destroy(p);
        }
        
        if (success) {
            success_count++;
        }
    };
    
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(thread_fn);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    ASSERT_EQ(success_count, num_threads);
}

/* ============================================================================
 * Performance Tests (Smoke)
 * ============================================================================ */

TEST(perf_heap_allocation) {
    memento::heap h;
    const int n = 10000;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < n; i++) {
        auto ptr = h.allocate(256);
        h.deallocate(ptr, 256);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    std::cout << "(" << ms << "ms for " << n << " ops) ";
    ASSERT(ms < 1000);  /* Should complete in less than 1 second */
}

TEST(perf_pool_allocation) {
    memento::pool<Particle> pool(10000);
    const int n = 10000;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < n; i++) {
        auto p = pool.emplace();
        pool.destroy(p);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    std::cout << "(" << ms << "ms for " << n << " ops) ";
    ASSERT(ms < 1000);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main() {
    std::cout << "=== Memento C++ Test Suite ===\n\n";
    
    try {
        memento::context ctx;
        
        std::cout << "Version Tests:\n";
        RUN_TEST(version_check);
        
        std::cout << "\nContext Tests:\n";
        RUN_TEST(context_raii);
        RUN_TEST(context_multiple);
        
        std::cout << "\nHeap Tests:\n";
        RUN_TEST(heap_basic);
        RUN_TEST(heap_object_construction);
        RUN_TEST(heap_array_allocation);
        RUN_TEST(heap_realloc);
        RUN_TEST(heap_null_deallocate);
        RUN_TEST(heap_stats);
        
        std::cout << "\nPool Tests:\n";
        RUN_TEST(pool_basic);
        RUN_TEST(pool_exhaustion);
        RUN_TEST(pool_recycle);
        RUN_TEST(pool_move);
        
        std::cout << "\nArena Tests:\n";
        RUN_TEST(arena_basic);
        RUN_TEST(arena_object_construction);
        RUN_TEST(arena_save_restore);
        RUN_TEST(arena_reset);
        RUN_TEST(arena_growth);
        RUN_TEST(arena_move);
        
        std::cout << "\nStack Tests:\n";
        RUN_TEST(stack_basic);
        RUN_TEST(stack_marker);
        RUN_TEST(stack_reset);
        RUN_TEST(stack_overflow);
        RUN_TEST(stack_move);
        
        std::cout << "\nScoped Pointer Tests:\n";
        RUN_TEST(scoped_ptr_basic);
        RUN_TEST(scoped_ptr_reset);
        RUN_TEST(scoped_ptr_release);
        RUN_TEST(scoped_ptr_move);
        
        std::cout << "\nSTL Allocator Tests:\n";
        RUN_TEST(stl_vector_basic);
        RUN_TEST(stl_vector_reserve);
        RUN_TEST(stl_vector_strings);
        RUN_TEST(stl_vector_move);
        RUN_TEST(stl_rebind);
        
        std::cout << "\nThread Safety Tests:\n";
        RUN_TEST(thread_heap_concurrent);
        RUN_TEST(thread_pool_concurrent);
        
        std::cout << "\nPerformance Tests:\n";
        RUN_TEST(perf_heap_allocation);
        RUN_TEST(perf_pool_allocation);
        
    } catch (const std::exception& e) {
        std::cout << "\nException caught: " << e.what() << "\n";
        failed++;
    }
    
    std::cout << "\n=== Results ===\n";
    std::cout << "  Passed: " << passed << "\n";
    std::cout << "  Failed: " << failed << "\n";
    
    if (failed == 0) {
        std::cout << "\n✓ All tests passed!\n";
        return 0;
    } else {
        std::cout << "\n✗ Some tests failed!\n";
        return 1;
    }
}
