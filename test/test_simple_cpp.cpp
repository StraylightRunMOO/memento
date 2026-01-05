#define MEMENTO_IMPLEMENTATION
#include "memento.h"
#include "memento_cpp.hpp"
#include <iostream>

int main() {
    std::cout << "Testing memento C++ wrapper..." << std::endl;
    
    // Check initial state
    std::cout << "Initial state: " << memento::is_initialized() << std::endl;
    
    try {
        // Test initialization
        std::cout << "Initializing..." << std::endl;
        memento::scoped_init init;
        std::cout << "Initialized successfully: " << memento::is_initialized() << std::endl;
        
        // Test basic allocation
        std::cout << "Creating allocator..." << std::endl;
        auto allocator = memento::allocator::create_thread_cache("test");
        std::cout << "Allocator created: " << allocator.name() << std::endl;
        
        // Test allocation
        std::cout << "Allocating memory..." << std::endl;
        void* ptr = allocator.allocate(1024);
        std::cout << "Allocated at: " << ptr << std::endl;
        
        // Test deallocation
        std::cout << "Deallocating memory..." << std::endl;
        allocator.deallocate(ptr);
        std::cout << "Deallocated successfully" << std::endl;
        
        std::cout << "All tests passed!" << std::endl;
        
    } catch (const std::exception& e) {
        std::cout << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}