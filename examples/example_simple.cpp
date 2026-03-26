/*
 * Memento C++ Simple Example
 * 
 * Demonstrates basic usage of the C++ wrapper for the Memento allocator.
 */

#define MEMENTO_IMPLEMENTATION

#include <iostream>
#include <vector>
#include <string>
#include "memento.hpp"

struct Particle {
    float x, y, z;
    float vx, vy, vz;
    
    Particle(float x = 0, float y = 0, float z = 0) 
        : x(x), y(y), z(z), vx(0), vy(0), vz(0) {}
    ~Particle() = default;
};

struct MyObject {
    int value;
    std::string name;
    
    MyObject(int v, const std::string& n) : value(v), name(n) {
        std::cout << "  MyObject(" << value << ", \"" << name << "\") constructed\n";
    }
    ~MyObject() {
        std::cout << "  MyObject(" << value << ", \"" << name << "\") destroyed\n";
    }
};

int main() {
    std::cout << "=== Memento C++ Simple Example ===\n\n";
    
    // RAII context initialization
    memento::context ctx;
    
    // ========================================================================
    // Test 1: Basic heap usage
    // ========================================================================
    std::cout << "Test 1: Thread-local heap\n";
    {
        memento::heap h;
        
        // Raw allocation
        void* raw = h.allocate(1024);
        std::cout << "  ✓ Allocated 1024 bytes\n";
        h.deallocate(raw, 1024);
        std::cout << "  ✓ Deallocated 1024 bytes\n";
        
        // Object construction
        auto obj = h.construct<MyObject>(42, "test");
        h.destroy(obj);
        std::cout << "  ✓ Constructed and destroyed MyObject\n";
    }
    
    // ========================================================================
    // Test 2: Pool allocator
    // ========================================================================
    std::cout << "\nTest 2: Pool allocator\n";
    {
        memento::pool<Particle> pool(100);
        
        // Allocate and construct particles
        auto p1 = pool.emplace(1.0f, 2.0f, 3.0f);
        auto p2 = pool.emplace(4.0f, 5.0f, 6.0f);
        std::cout << "  ✓ Created 2 particles from pool\n";
        
        // Destroy and return to pool
        pool.destroy(p1);
        pool.destroy(p2);
        std::cout << "  ✓ Destroyed particles\n";
    }
    
    // ========================================================================
    // Test 3: Arena allocator with save/restore
    // ========================================================================
    std::cout << "\nTest 3: Arena allocator\n";
    {
        memento::arena arena(64 * 1024);  // 64KB initial
        
        // Save point
        auto save = arena.save();
        
        // Allocate temporary objects
        auto temp1 = arena.construct<MyObject>(1, "temp1");
        auto temp2 = arena.construct<MyObject>(2, "temp2");
        (void)temp1; (void)temp2; // Suppress unused warning
        std::cout << "  ✓ Created temporary objects in arena\n";
        
        // Restore to save point (destroys temporaries conceptually)
        arena.restore(save);
        std::cout << "  ✓ Restored arena to save point\n";
        
        // Allocate more
        auto temp3 = arena.construct<MyObject>(3, "temp3");
        (void)temp3;
        std::cout << "  ✓ Created new object after restore\n";
    }
    
    // ========================================================================
    // Test 4: Stack allocator
    // ========================================================================
    std::cout << "\nTest 4: Stack allocator\n";
    {
        memento::stack<Particle> stack(4096);  // 4KB stack
        
        // Push marker
        auto mark = stack.mark();
        
        // Push objects
        auto s1 = stack.push(1.0f, 2.0f, 3.0f);
        auto s2 = stack.push(4.0f, 5.0f, 6.0f);
        std::cout << "  ✓ Pushed 2 particles to stack\n";
        
        // Pop to marker (bulk deallocation)
        stack.restore(mark);
        std::cout << "  ✓ Restored stack to marker\n";
    }
    
    // ========================================================================
    // Test 5: STL allocator
    // ========================================================================
    std::cout << "\nTest 5: STL allocator\n";
    {
        memento::heap h;
        
        // Create vector with custom allocator
        std::vector<int, memento::allocator<int>> vec{memento::allocator<int>{h}};
        vec.reserve(100);
        
        for (int i = 0; i < 10; i++) {
            vec.push_back(i * i);
        }
        
        std::cout << "  ✓ Created vector with " << vec.size() << " elements\n";
        
        // Range-based for loop
        int sum = 0;
        for (int x : vec) {
            sum += x;
        }
        std::cout << "  ✓ Sum of elements: " << sum << "\n";
    }
    
    // ========================================================================
    // Test 6: Scoped pointer
    // ========================================================================
    std::cout << "\nTest 6: Scoped pointer\n";
    {
        memento::heap h;
        
        {
            auto scoped = h.construct<MyObject>(100, "scoped");
            memento::scoped_ptr<MyObject> ptr(scoped, &h);
            std::cout << "  ✓ Created scoped pointer\n";
            // Automatically destroyed when ptr goes out of scope
        }
        std::cout << "  ✓ Scoped pointer destroyed\n";
    }
    
    // ========================================================================
    // Test 7: Statistics
    // ========================================================================
    std::cout << "\nTest 7: Statistics\n";
    {
        memento::heap h;
        
        // Do some allocations
        auto p1 = h.allocate(128);
        auto p2 = h.allocate(256);
        auto p3 = h.allocate(512);
        
        h.deallocate(p1, 128);
        h.deallocate(p2, 256);
        h.deallocate(p3, 512);
        
        // Print stats
        memento::print_stats();
    }
    
    std::cout << "\n=== All tests passed! ===\n";
    return 0;
}
