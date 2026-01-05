/*
 * C++ STL Integration Example
 * 
 * This example demonstrates how to use memento with STL containers
 * and algorithms for seamless integration with existing C++ code.
 */

#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <memory>
#include <chrono>

#define MEMENTO_IMPLEMENTATION
#include "../include/memento.h"
#include "../include/memento_cpp.hpp"

/* Simple data structures for demonstration */
struct Person {
    std::string name;
    int age;
    std::string city;
    
    Person(const std::string& n, int a, const std::string& c) 
        : name(n), age(a), city(c) {}
};

struct Product {
    int id;
    std::string name;
    double price;
    int quantity;
    
    Product(int i, const std::string& n, double p, int q)
        : id(i), name(n), price(p), quantity(q) {}
};

static void demonstrate_vector_with_custom_allocator(void) {
    std::cout << "\n2. Vector with Custom Allocator\n";
    std::cout << "   -----------------------------\n";
    
    auto allocator = memento::allocator::create_thread_cache("stl_vector");
    memento::stl_allocator<Person> person_alloc(&allocator);
    
    std::cout << "   Creating vector of Person objects...\n";
    std::vector<Person, memento::stl_allocator<Person>> people(person_alloc);
    
    /* Add people to the vector */
    people.emplace_back("Alice", 25, "New York");
    people.emplace_back("Bob", 30, "Los Angeles");
    people.emplace_back("Charlie", 35, "Chicago");
    people.emplace_back("Diana", 28, "Miami");
    
    std::cout << "   Added " << people.size() << " people to vector\n";
    
    /* Display the people */
    std::cout << "   People in vector:\n";
    for (const auto& person : people) {
        std::cout << "     - " << person.name << ", " << person.age 
                  << " years old, from " << person.city << "\n";
    }
    
    /* Sort the vector */
    std::sort(people.begin(), people.end(), 
              [](const Person& a, const Person& b) { return a.age < b.age; });
    
    std::cout << "   Sorted by age:\n";
    for (const auto& person : people) {
        std::cout << "     - " << person.name << " (" << person.age << ")\n";
    }
    
    auto stats = allocator.stats();
    std::cout << "   Vector statistics: " << stats.allocation_count << " allocations\n";
}

static void demonstrate_map_with_custom_allocator(void) {
    std::cout << "\n3. Map with Custom Allocator\n";
    std::cout << "   -------------------------\n";
    
    auto allocator = memento::allocator::create_thread_cache("stl_map");
    
    /* Create allocators for map components */
    memento::stl_allocator<std::pair<const int, Product>> pair_alloc(&allocator);
    memento::stl_allocator<Product> product_alloc(&allocator);
    
    std::cout << "   Creating map of Product objects...\n";
    std::map<int, Product, std::less<int>, memento::stl_allocator<std::pair<const int, Product>>> 
        inventory(pair_alloc);
    
    /* Add products to inventory */
    inventory.emplace(1001, Product(1001, "Laptop", 999.99, 5));
    inventory.emplace(1002, Product(1002, "Mouse", 29.99, 50));
    inventory.emplace(1003, Product(1003, "Keyboard", 79.99, 30));
    inventory.emplace(1004, Product(1004, "Monitor", 299.99, 10));
    
    std::cout << "   Added " << inventory.size() << " products to inventory\n";
    
    /* Display inventory */
    std::cout << "   Inventory:\n";
    for (const auto& [id, product] : inventory) {
        std::cout << "     - ID: " << product.id 
                  << ", Name: " << product.name
                  << ", Price: $" << product.price
                  << ", Quantity: " << product.quantity << "\n";
    }
    
    /* Calculate total inventory value */
    double total_value = 0.0;
    for (const auto& [id, product] : inventory) {
        total_value += product.price * product.quantity;
    }
    
    std::cout << "   Total inventory value: $" << total_value << "\n";
    
    auto stats = allocator.stats();
    std::cout << "   Map statistics: " << stats.allocation_count << " allocations\n";
}

static void demonstrate_string_with_custom_allocator(void) {
    std::cout << "\n4. String with Custom Allocator\n";
    std::cout << "   -----------------------------\n";
    
    auto allocator = memento::allocator::create_thread_cache("stl_string");
    memento::stl_allocator<char> char_alloc(&allocator);
    
    std::cout << "   Creating strings with custom allocator...\n";
    
    /* Create strings with custom allocator */
    std::basic_string<char, std::char_traits<char>, memento::stl_allocator<char>> 
        message(char_alloc);
    
    message = "Hello, Memento STL Integration!";
    std::cout << "   Message: " << message << "\n";
    std::cout << "   Length: " << message.length() << " characters\n";
    
    /* Build a larger string */
    std::basic_string<char, std::char_traits<char>, memento::stl_allocator<char>> 
        large_string(char_alloc);
    
    for (int i = 0; i < 10; i++) {
        large_string += "This is line " + std::to_string(i) + " of the large string. ";
    }
    
    std::cout << "   Large string length: " << large_string.length() << " characters\n";
    
    /* String operations */
    std::basic_string<char, std::char_traits<char>, memento::stl_allocator<char>> 
        upper_string(char_alloc);
    
    upper_string = message;
    std::transform(upper_string.begin(), upper_string.end(), upper_string.begin(), ::toupper);
    std::cout << "   Uppercase: " << upper_string << "\n";
    
    auto stats = allocator.stats();
    std::cout << "   String statistics: " << stats.allocation_count << " allocations\n";
}

static void demonstrate_complex_data_structures(void) {
    std::cout << "\n5. Complex Data Structures\n";
    std::cout << "   ------------------------\n";
    
    auto allocator = memento::allocator::create_thread_cache("stl_complex");
    
    /* Multi-level data structure */
    std::cout << "   Creating complex nested data structure...\n";
    
    using StringAlloc = memento::stl_allocator<char>;
    using PersonAlloc = memento::stl_allocator<Person>;
    using StringVectorAlloc = memento::stl_allocator<std::basic_string<char, std::char_traits<char>, StringAlloc>>;
    
    StringAlloc string_alloc(&allocator);
    PersonAlloc person_alloc(&allocator);
    StringVectorAlloc string_vector_alloc(&allocator);
    
    /* Create departments with employees */
    std::map<std::basic_string<char, std::char_traits<char>, StringAlloc>, 
             std::vector<Person, PersonAlloc>, 
             std::less<std::basic_string<char, std::char_traits<char>, StringAlloc>>,
             memento::stl_allocator<std::pair<const std::basic_string<char, std::char_traits<char>, StringAlloc>, 
                                               std::vector<Person, PersonAlloc>>>>
        departments(string_vector_alloc);
    
    /* Add departments and employees */
    departments["Engineering"].emplace_back("Alice", 30, "Software Engineer");
    departments["Engineering"].emplace_back("Bob", 35, "Senior Developer");
    departments["Engineering"].emplace_back("Charlie", 28, "DevOps Engineer");
    
    departments["Marketing"].emplace_back("Diana", 32, "Marketing Manager");
    departments["Marketing"].emplace_back("Eve", 29, "Content Creator");
    
    departments["Sales"].emplace_back("Frank", 38, "Sales Director");
    departments["Sales"].emplace_back("Grace", 31, "Account Manager");
    
    std::cout << "   Company structure:\n";
    for (const auto& [department, employees] : departments) {
        std::cout << "     Department: " << department << " (" << employees.size() << " employees)\n";
        for (const auto& employee : employees) {
            std::cout << "       - " << employee.name << ", " << employee.age 
                      << " years old, " << employee.city << "\n";
        }
    }
    
    auto stats = allocator.stats();
    std::cout << "   Complex structure statistics: " << stats.allocation_count << " allocations\n";
}

static void demonstrate_performance_comparison(void) {
    std::cout << "\n6. Performance Comparison\n";
    std::cout << "   -----------------------\n";
    
    auto allocator = memento::allocator::create_thread_cache("stl_perf");
    memento::stl_allocator<int> int_alloc(&allocator);
    
    const int iterations = 100000;
    const int vector_size = 1000;
    
    std::cout << "   Comparing STL vector performance (" << iterations << " operations):\n";
    
    /* Test with custom allocator */
    auto start = std::chrono::high_resolution_clock::now();
    
    std::vector<int, memento::stl_allocator<int>> custom_vector(int_alloc);
    custom_vector.reserve(vector_size);
    
    for (int i = 0; i < iterations; i++) {
        custom_vector.push_back(i);
        if (custom_vector.size() > vector_size) {
            custom_vector.clear();
        }
    }
    
    auto custom_time = std::chrono::high_resolution_clock::now() - start;
    
    /* Test with standard allocator */
    start = std::chrono::high_resolution_clock::now();
    
    std::vector<int> standard_vector;
    standard_vector.reserve(vector_size);
    
    for (int i = 0; i < iterations; i++) {
        standard_vector.push_back(i);
        if (standard_vector.size() > vector_size) {
            standard_vector.clear();
        }
    }
    
    auto standard_time = std::chrono::high_resolution_clock::now() - start;
    
    auto custom_us = std::chrono::duration_cast<std::chrono::microseconds>(custom_time).count();
    auto standard_us = std::chrono::duration_cast<std::chrono::microseconds>(standard_time).count();
    
    std::cout << "   Custom allocator: " << custom_us << " μs\n";
    std::cout << "   Standard allocator: " << standard_us << " μs\n";
    std::cout << "   Performance difference: " 
              << (custom_us > standard_us ? "slower" : "faster") 
              << " by " << std::abs(custom_us - standard_us) << " μs\n";
    
    auto stats = allocator.stats();
    std::cout << "   Performance test statistics: " << stats.allocation_count << " allocations\n";
}

static void demonstrate_memory_efficiency(void) {
    std::cout << "\n7. Memory Efficiency\n";
    std::cout << "   ------------------\n";
    
    auto allocator = memento::allocator::create_thread_cache("stl_efficiency");
    
    std::cout << "   Demonstrating memory-efficient STL usage...\n";
    
    /* String optimization with custom allocator */
    memento::stl_allocator<char> char_alloc(&allocator);
    
    std::cout << "   Creating short strings with SSO (Small String Optimization):\n";
    std::basic_string<char, std::char_traits<char>, memento::stl_allocator<char>> 
        short_string(char_alloc);
    
    short_string = "Short";
    std::cout << "     Short string: '" << short_string << "' (may not allocate)\n";
    
    std::basic_string<char, std::char_traits<char>, memento::stl_allocator<char>> 
        long_string(char_alloc);
    
    long_string = "This is a very long string that will definitely require heap allocation";
    std::cout << "     Long string: length = " << long_string.length() 
              << " (will allocate)\n";
    
    /* Vector capacity management */
    memento::stl_allocator<int> int_alloc(&allocator);
    
    std::cout << "   Vector capacity management:\n";
    std::vector<int, memento::stl_allocator<int>> numbers(int_alloc);
    
    std::cout << "     Initial capacity: " << numbers.capacity() << "\n";
    
    /* Reserve space to avoid reallocations */
    numbers.reserve(100);
    std::cout << "     After reserve(100): capacity = " << numbers.capacity() << "\n";
    
    for (int i = 0; i < 100; i++) {
        numbers.push_back(i);
    }
    
    std::cout << "     After adding 100 elements: capacity = " << numbers.capacity() << "\n";
    
    /* Shrink to fit */
    numbers.shrink_to_fit();
    std::cout << "     After shrink_to_fit(): capacity = " << numbers.capacity() << "\n";
    
    auto stats = allocator.stats();
    std::cout << "   Memory efficiency statistics: " << stats.allocation_count << " allocations\n";
}

int main(void) {
    std::cout << "Memento C++ STL Integration Example\n";
    std::cout << "===================================\n\n";
    
    try {
        /* RAII initialization */
        memento::scoped_init init;
        
        std::cout << "1. Initializing with RAII...\n";
        
        /* Demonstrate different STL integration patterns */
        demonstrate_vector_with_custom_allocator();
        demonstrate_map_with_custom_allocator();
        demonstrate_string_with_custom_allocator();
        demonstrate_complex_data_structures();
        demonstrate_performance_comparison();
        demonstrate_memory_efficiency();
        
        std::cout << "\n✓ C++ STL integration example completed successfully!\n";
        std::cout << "\nKey takeaways:\n";
        std::cout << "- Memento provides STL-compatible allocators\n";
        std::cout << "- Works with all STL containers (vector, map, string, etc.)\n";
        std::cout << "- Maintains type safety and performance\n";
        std::cout << "- Seamless integration with existing C++ code\n";
        std::cout << "- Memory usage tracking and statistics\n";
        
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
