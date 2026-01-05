/*
 * C++ Custom Integration Example
 * 
 * This example demonstrates advanced C++ integration techniques
 * including custom allocators, memory pools, and specialized containers.
 */

#include <iostream>
#include <vector>
#include <memory>
#include <type_traits>
#include <new>

#define MEMENTO_IMPLEMENTATION
#include "../include/memento.h"
#include "../include/memento_cpp.hpp"

/* Custom memory pool for specific object types */
template<typename T>
class MemoryPool {
private:
    memento::allocator allocator_;
    std::vector<T*> available_objects_;
    std::vector<std::unique_ptr<T, std::function<void(T*)>>> all_objects_;
    
public:
    explicit MemoryPool(const std::string& name, size_t initial_size = 100)
        : allocator_(memento::allocator::create_thread_cache(name.c_str())) {
        
        /* Pre-allocate objects */
        available_objects_.reserve(initial_size);
        all_objects_.reserve(initial_size);
        
        for (size_t i = 0; i < initial_size; ++i) {
            T* obj = allocator_.construct<T>();
            available_objects_.push_back(obj);
            
            /* Store with custom deleter that returns to pool */
            all_objects_.emplace_back(obj, [this](T* ptr) {
                /* Reset object state */
                ptr->~T();
                new (ptr) T();
                available_objects_.push_back(ptr);
            });
        }
    }
    
    ~MemoryPool() {
        /* All objects will be properly destroyed when pool is destroyed */
    }
    
    std::unique_ptr<T, std::function<void(T*)>> acquire() {
        if (available_objects_.empty()) {
            /* Pool exhausted, allocate new object */
            T* obj = allocator_.construct<T>();
            return std::unique_ptr<T, std::function<void(T*)>>(obj, 
                [this](T* ptr) { allocator_.destroy(ptr); });
        }
        
        T* obj = available_objects_.back();
        available_objects_.pop_back();
        
        /* Return the pre-allocated object with custom deleter */
        return std::unique_ptr<T, std::function<void(T*)>>(obj, 
            [](T* /* ptr */) { /* Object returned to pool automatically */ });
    }
    
    size_t available() const { return available_objects_.size(); }
    size_t total() const { return all_objects_.size(); }
    
    void printStats() const {
        auto stats = allocator_.stats();
        std::cout << "   Memory Pool Stats:\n";
        std::cout << "     Available objects: " << available() << "\n";
        std::cout << "     Total objects: " << total() << "\n";
        std::cout << "     Allocator allocations: " << stats.allocation_count << "\n";
    }
};

/* Game entity that can be pooled */
class PooledEntity {
private:
    int id_;
    bool active_;
    
public:
    PooledEntity() : id_(0), active_(false) {}
    
    void initialize(int id) {
        id_ = id;
        active_ = true;
    }
    
    void reset() {
        id_ = 0;
        active_ = false;
    }
    
    void update(float /* delta_time */) {
        if (active_) {
            /* Entity update logic */
            std::cout << "     PooledEntity " << id_ << " updated\n";
        }
    }
    
    void render() const {
        if (active_) {
            std::cout << "     PooledEntity " << id_ << " rendered\n";
        }
    }
    
    int getId() const { return id_; }
    bool isActive() const { return active_; }
};

/* Custom vector implementation with memento allocator */
template<typename T>
class CustomVector {
private:
    T* data_;
    size_t size_;
    size_t capacity_;
    memento::allocator* allocator_;
    
public:
    explicit CustomVector(memento::allocator* alloc) 
        : data_(nullptr), size_(0), capacity_(0), allocator_(alloc) {}
    
    ~CustomVector() {
        clear();
    }
    
    /* No copy semantics - move only */
    CustomVector(const CustomVector&) = delete;
    CustomVector& operator=(const CustomVector&) = delete;
    
    CustomVector(CustomVector&& other) noexcept
        : data_(other.data_), size_(other.size_), 
          capacity_(other.capacity_), allocator_(other.allocator_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }
    
    void push_back(const T& value) {
        if (size_ >= capacity_) {
            reserve(capacity_ == 0 ? 1 : capacity_ * 2);
        }
        
        new (&data_[size_]) T(value);
        size_++;
    }
    
    template<typename... Args>
    void emplace_back(Args&&... args) {
        if (size_ >= capacity_) {
            reserve(capacity_ == 0 ? 1 : capacity_ * 2);
        }
        
        new (&data_[size_]) T(std::forward<Args>(args)...);
        size_++;
    }
    
    void reserve(size_t new_capacity) {
        if (new_capacity <= capacity_) return;
        
        T* new_data = allocator_->allocate_object<T>(new_capacity);
        
        /* Move existing elements */
        for (size_t i = 0; i < size_; ++i) {
            new (new_data + i) T(std::move(data_[i]));
            data_[i].~T();
        }
        
        /* Free old data */
        if (data_) {
            for (size_t i = 0; i < size_; ++i) {
                allocator_->destroy(&data_[i]);
            }
            allocator_->deallocate(data_);
        }
        
        data_ = new_data;
        capacity_ = new_capacity;
    }
    
    void clear() {
        if (data_) {
            for (size_t i = 0; i < size_; ++i) {
                allocator_->destroy(&data_[i]);
            }
            allocator_->deallocate(data_);
            data_ = nullptr;
        }
        size_ = 0;
        capacity_ = 0;
    }
    
    T& operator[](size_t index) { return data_[index]; }
    const T& operator[](size_t index) const { return data_[index]; }
    
    size_t size() const { return size_; }
    size_t capacity() const { return capacity_; }
    bool empty() const { return size_ == 0; }
    
    T* begin() { return data_; }
    T* end() { return data_ + size_; }
    const T* begin() const { return data_; }
    const T* end() const { return data_ + size_; }
};

/* RAII wrapper for scoped memory allocation */
class ScopedMemory {
private:
    memento::allocator allocator_;
    void* ptr_;
    size_t size_;
    
public:
    ScopedMemory(const std::string& name, size_t size)
        : allocator_(memento::allocator::create_thread_cache(name.c_str()))
        , ptr_(nullptr)
        , size_(size) {
        
        ptr_ = allocator_.allocate(size);
        if (ptr_) {
            std::cout << "   ScopedMemory allocated " << size << " bytes at " << ptr_ << "\n";
        } else {
            throw std::runtime_error("Failed to allocate scoped memory");
        }
    }
    
    ~ScopedMemory() {
        if (ptr_) {
            allocator_.deallocate(ptr_);
            std::cout << "   ScopedMemory freed " << size_ << " bytes\n";
        }
    }
    
    /* No copy or move */
    ScopedMemory(const ScopedMemory&) = delete;
    ScopedMemory& operator=(const ScopedMemory&) = delete;
    ScopedMemory(ScopedMemory&&) = delete;
    ScopedMemory& operator=(ScopedMemory&&) = delete;
    
    void* get() { return ptr_; }
    size_t size() const { return size_; }
    
    template<typename T>
    T* as() { return static_cast<T*>(ptr_); }
};

/* Custom memory tracking wrapper */
class TrackedAllocation {
private:
    static std::atomic<size_t> total_allocated_;
    static std::atomic<size_t> total_deallocated_;
    static std::atomic<size_t> active_allocations_;
    
    memento::allocator allocator_;
    void* ptr_;
    size_t size_;
    const std::string name_;
    
public:
    TrackedAllocation(const std::string& name, size_t size)
        : allocator_(memento::allocator::create_proxy(name.c_str()))
        , ptr_(nullptr)
        , size_(size)
        , name_(name) {
        
        total_allocated_ += size;
        active_allocations_++;
        
        ptr_ = allocator_.allocate(size);
        if (ptr_) {
            std::cout << "   TrackedAllocation '" << name_ << "' created: " << size << " bytes\n";
        } else {
            throw std::runtime_error("Failed to create tracked allocation");
        }
    }
    
    ~TrackedAllocation() {
        if (ptr_) {
            allocator_.deallocate(ptr_);
            total_deallocated_ += size_;
            active_allocations_--;
            std::cout << "   TrackedAllocation '" << name_ << "' destroyed: " << size_ << " bytes\n";
        }
    }
    
    static void printGlobalStats() {
        std::cout << "\n   Global Allocation Statistics:\n";
        std::cout << "     Total allocated: " << total_allocated_.load() << " bytes\n";
        std::cout << "     Total deallocated: " << total_deallocated_.load() << " bytes\n";
        std::cout << "     Active allocations: " << active_allocations_.load() << "\n";
        std::cout << "     Memory in use: " << (total_allocated_.load() - total_deallocated_.load()) << " bytes\n";
    }
    
    void printLocalStats() const {
        auto stats = allocator_.stats();
        std::cout << "   Allocation '" << name_ << "' statistics:\n";
        std::cout << "     Size: " << size_ << " bytes\n";
        std::cout << "     Allocator allocations: " << stats.allocation_count << "\n";
        std::cout << "     Current usage: " << stats.current_usage << " bytes\n";
    }
    
    /* Access the allocated memory */
    template<typename T>
    T* as() { return static_cast<T*>(ptr_); }
    
    const std::string& getName() const { return name_; }
    size_t getSize() const { return size_; }
};

/* Initialize static members */
std::atomic<size_t> TrackedAllocation::total_allocated_{0};
std::atomic<size_t> TrackedAllocation::total_deallocated_{0};
std::atomic<size_t> TrackedAllocation::active_allocations_{0};

static void demonstrate_memory_pool(void) {
    std::cout << "\n2. Memory Pool Demonstration\n";
    std::cout << "   -------------------------\n";
    
    std::cout << "   Creating memory pool for PooledEntity objects...\n";
    MemoryPool<PooledEntity> entity_pool("entity_pool", 10);
    
    std::cout << "   Initial pool state:\n";
    entity_pool.printStats();
    
    std::cout << "   Acquiring entities from pool...\n";
    auto entity1 = entity_pool.acquire();
    auto entity2 = entity_pool.acquire();
    auto entity3 = entity_pool.acquire();
    
    if (entity1 && entity2 && entity3) {
        entity1->initialize(1);
        entity2->initialize(2);
        entity3->initialize(3);
        
        std::cout << "   Using entities...\n";
        entity1->update(0.1f);
        entity2->render();
        entity3->update(0.2f);
        
        /* Entities automatically return to pool when unique_ptr is destroyed */
    }
    
    std::cout << "   Pool state after use:\n";
    entity_pool.printStats();
    
    std::cout << "   Memory pool demo completed\n";
}

static void demonstrate_custom_vector(void) {
    std::cout << "\n3. Custom Vector Implementation\n";
    std::cout << "   -----------------------------\n";
    
    auto allocator = memento::allocator::create_thread_cache("custom_vector");
    
    std::cout << "   Creating custom vector with memento allocator...\n";
    CustomVector<int> custom_vector(&allocator);
    
    std::cout << "   Adding elements to vector...\n";
    for (int i = 0; i < 10; i++) {
        custom_vector.push_back(i * i);
    }
    
    std::cout << "   Vector contents: ";
    for (size_t i = 0; i < custom_vector.size(); i++) {
        std::cout << custom_vector[i] << " ";
    }
    std::cout << "\n";
    
    std::cout << "   Vector capacity: " << custom_vector.capacity() << "\n";
    std::cout << "   Vector size: " << custom_vector.size() << "\n";
    
    /* Test with custom objects */
    CustomVector<std::string> string_vector(&allocator);
    string_vector.push_back("Hello");
    string_vector.push_back("Memento");
    string_vector.push_back("Custom");
    string_vector.push_back("Vector");
    
    std::cout << "   String vector contents: ";
    for (size_t i = 0; i < string_vector.size(); i++) {
        std::cout << string_vector[i] << " ";
    }
    std::cout << "\n";
    
    auto stats = allocator.stats();
    std::cout << "   Custom vector statistics: " << stats.allocation_count << " allocations\n";
    
    std::cout << "   Custom vector demo completed\n";
}

static void demonstrate_scoped_memory(void) {
    std::cout << "\n4. Scoped Memory Demonstration\n";
    std::cout << "   ----------------------------\n";
    
    std::cout << "   Using scoped memory allocation...\n";
    
    /* Allocate scoped memory */
    {
        ScopedMemory buffer("render_buffer", 1024 * 1024); /* 1MB */
        
        std::cout << "   Using scoped memory for rendering...\n";
        
        /* Use the memory */
        float* render_data = buffer.as<float>();
        for (size_t i = 0; i < buffer.size() / sizeof(float); i++) {
            render_data[i] = static_cast<float>(i) * 0.001f;
        }
        
        std::cout << "   Render data initialized: " << render_data[0] 
                  << " to " << render_data[100] << "\n";
        
        /* Memory will be automatically freed when buffer goes out of scope */
    }
    
    std::cout << "   Scoped memory demo completed\n";
}

static void demonstrate_tracked_allocation(void) {
    std::cout << "\n5. Tracked Allocation Demonstration\n";
    std::cout << "   ---------------------------------\n";
    
    std::cout << "   Using tracked allocations with detailed statistics...\n";
    
    /* Create tracked allocations */
    {
        TrackedAllocation texture_data("texture_data", 4 * 1024 * 1024); /* 4MB */
        TrackedAllocation vertex_buffer("vertex_buffer", 2 * 1024 * 1024); /* 2MB */
        TrackedAllocation index_buffer("index_buffer", 1 * 1024 * 1024); /* 1MB */
        
        std::cout << "   Using tracked allocations...\n";
        
        /* Use the allocations */
        unsigned char* tex_data = texture_data.as<unsigned char>();
        float* vert_buffer = vertex_buffer.as<float>();
        uint32_t* idx_buffer = index_buffer.as<uint32_t>();
        
        /* Initialize some data */
        for (size_t i = 0; i < 100; i++) {
            tex_data[i] = static_cast<unsigned char>(i % 256);
            vert_buffer[i] = static_cast<float>(i) * 0.1f;
            idx_buffer[i] = static_cast<uint32_t>(i);
        }
        
        std::cout << "   Local statistics:\n";
        texture_data.printLocalStats();
        vertex_buffer.printLocalStats();
        index_buffer.printLocalStats();
        
        /* Tracked allocations will be automatically destroyed */
    }
    
    std::cout << "   Global statistics:\n";
    TrackedAllocation::printGlobalStats();
    
    std::cout << "   Tracked allocation demo completed\n";
}

int main(void) {
    std::cout << "Memento C++ Custom Integration Example\n";
    std::cout << "======================================\n\n";
    
    try {
        /* RAII initialization */
        memento::scoped_init init;
        
        std::cout << "1. Initializing with RAII...\n";
        
        /* Demonstrate advanced C++ integration techniques */
        demonstrate_memory_pool();
        demonstrate_custom_vector();
        demonstrate_scoped_memory();
        demonstrate_tracked_allocation();
        
        std::cout << "\n✓ C++ custom integration example completed successfully!\n";
        std::cout << "\nKey takeaways:\n";
        std::cout << "- Memory pools provide efficient object reuse\n";
        std::cout << "- Custom containers can integrate with memento\n";
        std::cout << "- RAII ensures proper resource management\n";
        std::cout << "- Detailed allocation tracking is available\n";
        std::cout << "- Advanced C++ patterns work seamlessly with memento\n";
        
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
