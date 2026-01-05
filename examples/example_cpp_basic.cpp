/*
 * Basic C++ usage example for memento allocator
 * 
 * This example demonstrates the C++ wrapper with RAII, templates,
 * and modern C++ features.
 */

#include <iostream>
#include <vector>
#include <string>
#include <memory>

#define MEMENTO_IMPLEMENTATION
#include "../include/memento.h"
#include "../include/memento_cpp.hpp"

class GameObject {
private:
    std::string name_;
    int health_;
    float position_[3];
    
public:
    GameObject(const std::string& name, int health, float x, float y, float z)
        : name_(name), health_(health) {
        position_[0] = x;
        position_[1] = y;
        position_[2] = z;
        
        std::cout << "    GameObject created: " << name_ << std::endl;
    }
    
    ~GameObject() {
        std::cout << "    GameObject destroyed: " << name_ << std::endl;
    }
    
    const std::string& getName() const { return name_; }
    int getHealth() const { return health_; }
    void setHealth(int health) { health_ = health; }
    
    void move(float dx, float dy, float dz) {
        position_[0] += dx;
        position_[1] += dy;
        position_[2] += dz;
    }
    
    void printInfo() const {
        std::cout << "      Name: " << name_ 
                  << ", Health: " << health_ 
                  << ", Position: (" << position_[0] 
                  << ", " << position_[1] 
                  << ", " << position_[2] << ")" << std::endl;
    }
};

void demonstrate_basic_allocation() {
    std::cout << "\n2. Basic Allocation Demo" << std::endl;
    std::cout << "   ---------------------" << std::endl;
    
    /* Create a thread cache allocator */
    auto allocator = memento::allocator::create_thread_cache("cpp_basic");
    std::cout << "   Created allocator: " << allocator.name() << std::endl;
    
    /* Allocate primitive types */
    int* number = allocator.allocate_object<int>();
    *number = 42;
    std::cout << "   Allocated int: " << *number << std::endl;
    allocator.deallocate(number);
    
    /* Allocate arrays */
    double* array = allocator.allocate_object<double>(10);
    for (int i = 0; i < 10; i++) {
        array[i] = i * 1.5;
    }
    std::cout << "   Allocated double array: [";
    for (int i = 0; i < 10; i++) {
        std::cout << array[i] << (i < 9 ? ", " : "");
    }
    std::cout << "]" << std::endl;
    allocator.deallocate(array);
    
    /* Show statistics */
    auto stats = allocator.stats();
    std::cout << "   Allocations: " << stats.allocation_count << std::endl;
    std::cout << "   Deallocations: " << stats.deallocation_count << std::endl;
}

void demonstrate_object_construction() {
    std::cout << "\n3. Object Construction Demo" << std::endl;
    std::cout << "   ------------------------" << std::endl;
    
    auto allocator = memento::allocator::create_thread_cache("cpp_objects");
    
    /* Construct objects with placement new */
    auto player = allocator.construct<GameObject>("Player", 100, 0.0f, 0.0f, 0.0f);
    auto enemy = allocator.construct<GameObject>("Enemy", 50, 10.0f, 0.0f, 5.0f);
    
    /* Use the objects */
    std::cout << "   Using objects:" << std::endl;
    player->printInfo();
    enemy->printInfo();
    
    player->move(1.0f, 2.0f, 3.0f);
    player->setHealth(95);
    std::cout << "   After moving and taking damage:" << std::endl;
    player->printInfo();
    
    /* Destroy objects (calls destructor) */
    std::cout << "   Destroying objects:" << std::endl;
    allocator.destroy(player);
    allocator.destroy(enemy);
}

void demonstrate_unique_ptr() {
    std::cout << "\n4. Unique Pointer Demo" << std::endl;
    std::cout << "   -------------------" << std::endl;
    
    auto allocator = memento::allocator::create_thread_cache("cpp_unique_ptr");
    
    /* Create unique_ptr with custom deleter */
    {
        std::cout << "   Creating unique_ptr..." << std::endl;
        auto obj = allocator.construct<GameObject>("UniqueObject", 75, 5.0f, 5.0f, 5.0f);
        memento::unique_ptr<GameObject> uptr(obj, &allocator);
        
        std::cout << "   Using object through unique_ptr:" << std::endl;
        uptr->printInfo();
        
        std::cout << "   unique_ptr will be automatically destroyed when going out of scope" << std::endl;
    } /* unique_ptr destructor calls GameObject destructor and frees memory */
    
    std::cout << "   unique_ptr destroyed (object and memory cleaned up)" << std::endl;
}

void demonstrate_stl_integration() {
    std::cout << "\n5. STL Integration Demo" << std::endl;
    std::cout << "   --------------------" << std::endl;
    
    auto allocator = memento::allocator::create_thread_cache("cpp_stl");
    
    /* Create STL-compatible allocator */
    memento::stl_allocator<int> int_alloc(&allocator);
    
    /* Use with std::vector */
    std::vector<int, memento::stl_allocator<int>> numbers(int_alloc);
    
    std::cout << "   Creating vector with custom allocator..." << std::endl;
    for (int i = 0; i < 10; i++) {
        numbers.push_back(i * i);
    }
    
    std::cout << "   Vector contents: [";
    for (size_t i = 0; i < numbers.size(); i++) {
        std::cout << numbers[i] << (i < numbers.size() - 1 ? ", " : "");
    }
    std::cout << "]" << std::endl;
    
    /* Use with simple string-like operations */
    std::cout << "   Creating string-like data with custom allocator...\n";
    
    /* Create simple string data */
    std::string message = "Hello, Memento STL Integration!";
    std::cout << "   Message: " << message << "\n";
    std::cout << "   Length: " << message.length() << " characters\n";
    
    /* String operations */
    std::string upper_string = message;
    std::transform(upper_string.begin(), upper_string.end(), upper_string.begin(), ::toupper);
    std::cout << "   Uppercase: " << upper_string << "\n";
}

void demonstrate_error_handling() {
    std::cout << "\n6. Error Handling Demo" << std::endl;
    std::cout << "   -------------------" << std::endl;
    
    auto allocator = memento::allocator::create_thread_cache("cpp_errors");
    
    /* Try invalid alignment */
    try {
        std::cout << "   Trying invalid alignment (3 bytes)..." << std::endl;
        static_cast<void>(allocator.allocate(100, 3));  /* Not power of two */
        std::cout << "   ERROR: Should have thrown exception!" << std::endl;
    } catch (const std::invalid_argument& e) {
        std::cout << "   Caught expected exception: " << e.what() << std::endl;
    }
    
    /* Try excessive size */
    try {
        std::cout << "   Trying excessive allocation size..." << std::endl;
        static_cast<void>(allocator.allocate(SIZE_MAX));
        std::cout << "   ERROR: Should have thrown exception!" << std::endl;
    } catch (const memento::allocation_error& e) {
        std::cout << "   Caught allocation error: " << e.what() << std::endl;
        std::cout << "   Requested size: " << e.requested_size() << std::endl;
        std::cout << "   Allocator name: " << e.allocator_name() << std::endl;
    }
    
    /* Try null allocator */
    try {
        std::cout << "   Trying to create allocator with null pointer..." << std::endl;
        memento::allocator bad_alloc(nullptr);
        std::cout << "   ERROR: Should have thrown exception!" << std::endl;
    } catch (const std::invalid_argument& e) {
        std::cout << "   Caught expected exception: " << e.what() << std::endl;
    }
}

int main(void) {
    std::cout << "Memento C++ Basic Example\n";
    std::cout << "=========================\n\n";
    
    try {
        /* RAII initialization - automatically cleans up */
        std::cout << "1. Initializing memento with RAII..." << std::endl;
        memento::scoped_init init;
        std::cout << "   Memento initialized successfully\n" << std::endl;
        
        /* Run all demonstrations */
        demonstrate_basic_allocation();
        demonstrate_object_construction();
        demonstrate_unique_ptr();
        demonstrate_stl_integration();
        demonstrate_error_handling();
        
        std::cout << "\n✓ C++ basic example completed successfully!" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\nUnknown error occurred!" << std::endl;
        return 1;
    }
    
    return 0;
}
