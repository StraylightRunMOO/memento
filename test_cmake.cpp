#define MEMENTO_IMPLEMENTATION
#include "memento.h"
#include "memento_cpp.hpp"
#include <iostream>

int main() {
    std::cout << "Testing CMake build..." << std::endl;
    
    try {
        memento::scoped_init init;
        
        auto allocator = memento::allocator::create_thread_cache("cmake_test");
        void* ptr = allocator.allocate(1024);
        
        std::cout << "Successfully allocated 1024 bytes at " << ptr << std::endl;
        
        allocator.deallocate(ptr);
        std::cout << "Successfully deallocated memory" << std::endl;
        
        std::cout << "CMake build test PASSED!" << std::endl;
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Test FAILED: " << e.what() << std::endl;
        return 1;
    }
}