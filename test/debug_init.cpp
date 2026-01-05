#define MEMENTO_IMPLEMENTATION
#include "memento.h"
#include "memento_cpp.hpp"
#include <iostream>

int main() {
    std::cout << "=== Debug Initialization Test ===" << std::endl;
    
    std::cout << "1. Initial state: " << memento::is_initialized() << std::endl;
    
    {
        std::cout << "2. Creating scoped_init..." << std::endl;
        memento::scoped_init init;
        std::cout << "3. After scoped_init: " << memento::is_initialized() << std::endl;
    }
    
    std::cout << "4. After scoped_init destroyed: " << memento::is_initialized() << std::endl;
    
    if (memento::is_initialized()) {
        std::cout << "5. Shutting down..." << std::endl;
        memento::shutdown();
        std::cout << "6. After shutdown: " << memento::is_initialized() << std::endl;
    }
    
    return 0;
}