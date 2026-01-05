/*
 * Memento Memory Allocator Library - C++ Wrapper
 * 
 * Modern C++17 wrapper for the memento C99 allocator library.
 * Provides RAII, templates, and STL-compatible interfaces.
 */

#ifndef MEMENTO_CPP_HPP
#define MEMENTO_CPP_HPP

#include "memento.h"
#include <memory>
#include <type_traits>
#include <utility>
#include <new>
#include <stdexcept>
#include <string>
#include <iostream>
#include <atomic>
#include <mutex>

namespace memento {

// Forward declarations
class allocator;
template<typename T> class arena;
template<typename T> struct stl_allocator;
template<typename T> class unique_ptr;

// Exception types
class allocation_error : public std::bad_alloc {
private:
    size_t requested_size_;
    size_t alignment_;
    std::string allocator_name_;
    
public:
    allocation_error(size_t size, size_t alignment, const char* allocator_name)
        : requested_size_(size), alignment_(alignment), allocator_name_(allocator_name ? allocator_name : "unknown") {}
    
    const char* what() const noexcept override {
        static thread_local char buffer[256];
        snprintf(buffer, sizeof(buffer), 
                 "Memento allocation failed: size=%zu, alignment=%zu, allocator=%s",
                 requested_size_, alignment_, allocator_name_.c_str());
        return buffer;
    }
    
    size_t requested_size() const noexcept { return requested_size_; }
    size_t alignment() const noexcept { return alignment_; }
    const std::string& allocator_name() const noexcept { return allocator_name_; }
};

class initialization_error : public std::runtime_error {
public:
    initialization_error(const char* msg) : std::runtime_error(msg) {}
};

// RAII initialization guard
class scoped_init {
private:
    bool initialized_by_us_;
    
public:
    scoped_init() : initialized_by_us_(false) {
        if (!memento_is_initialized()) {
            if (memento_init() != MEMENTO_SUCCESS) {
                throw initialization_error("Failed to initialize memento library");
            }
            initialized_by_us_ = true;
        }
    }
    
    ~scoped_init() {
        if (initialized_by_us_) {
            memento_shutdown();
        }
    }
    
    // Non-copyable
    scoped_init(const scoped_init&) = delete;
    scoped_init& operator=(const scoped_init&) = delete;
    
    // Non-movable (initialization should be persistent)
    scoped_init(scoped_init&&) = delete;
    scoped_init& operator=(scoped_init&&) = delete;
};

// Main allocator class
class allocator {
private:
    memento_allocator_t* allocator_;
    bool owns_allocator_;
    
public:
    // Constructors
    explicit allocator(memento_allocator_t* alloc, bool owns = false)
        : allocator_(alloc), owns_allocator_(owns) {
        if (!allocator_) {
            throw std::invalid_argument("Null allocator pointer");
        }
    }
    
    allocator() noexcept : allocator_(nullptr), owns_allocator_(false) {
        if (memento_is_initialized()) {
            /* Use root allocator by default */
            allocator_ = get_root_allocator();
            owns_allocator_ = false;
        }
    }
    
    // Destructor
    ~allocator() {
        if (owns_allocator_ && allocator_) {
            memento_destroy_allocator(allocator_);
        }
    }
    
    // Move semantics
    allocator(allocator&& other) noexcept
        : allocator_(other.allocator_), owns_allocator_(other.owns_allocator_) {
        other.allocator_ = nullptr;
        other.owns_allocator_ = false;
    }
    
    allocator& operator=(allocator&& other) noexcept {
        if (this != &other) {
            if (owns_allocator_ && allocator_) {
                memento_destroy_allocator(allocator_);
            }
            allocator_ = other.allocator_;
            owns_allocator_ = other.owns_allocator_;
            other.allocator_ = nullptr;
            other.owns_allocator_ = false;
        }
        return *this;
    }
    
    // No copy semantics
    allocator(const allocator&) = delete;
    allocator& operator=(const allocator&) = delete;
    
    // Core allocation functions
    [[nodiscard]] void* allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        if (!allocator_) {
            throw allocation_error(size, alignment, "null_allocator");
        }
        
        if (size == 0) {
            return nullptr;
        }
        
        /* Validate alignment */
        if (!is_power_of_two(alignment) || alignment > MEMENTO_MAX_ALIGNMENT) {
            throw std::invalid_argument("Invalid alignment");
        }
        
        memento_result_t result = memento_alloc_aligned(allocator_, size, alignment);
        if (!result.success || !result.ptr) {
            throw allocation_error(size, alignment, allocator_->name);
        }
        
        return result.ptr;
    }
    
    void deallocate(void* ptr) noexcept {
        if (ptr && allocator_) {
            memento_free(allocator_, ptr);
        }
    }
    
    // Template allocation helpers
    template<typename T>
    [[nodiscard]] T* allocate_object(size_t count = 1) {
        return static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
    }
    
    template<typename T, typename... Args>
    [[nodiscard]] T* construct(Args&&... args) {
        void* ptr = allocate_object<T>();
        try {
            return ::new(ptr) T(std::forward<Args>(args)...);
        } catch (...) {
            deallocate(ptr);
            throw;
        }
    }
    
    template<typename T>
    void destroy(T* ptr) noexcept {
        if (ptr) {
            ptr->~T();
            deallocate(ptr);
        }
    }
    
    // Statistics
    memento_stats_t stats() const {
        if (!allocator_) {
            return memento_stats_t{};
        }
        
        const memento_stats_t* stats = memento_get_stats(allocator_);
        return stats ? *stats : memento_stats_t{};
    }
    
    void print_stats() const {
        if (allocator_) {
            memento_print_stats(allocator_);
        }
    }
    
    const char* name() const noexcept {
        return allocator_ ? allocator_->name : "null_allocator";
    }
    
    // Factory methods
    static allocator create_thread_cache(const char* name = "thread_cache") {
        memento_allocator_t* alloc = memento_create_thread_cache(name);
        if (!alloc) {
            throw allocation_error(0, 0, name);
        }
        return allocator(alloc, true);
    }
    
    static allocator create_block(const char* name = "block", allocator* backing = nullptr) {
        memento_allocator_t* backing_alloc = backing ? backing->allocator_ : get_root_allocator();
        memento_allocator_t* alloc = memento_create_block_allocator(name, backing_alloc);
        if (!alloc) {
            throw allocation_error(0, 0, name);
        }
        return allocator(alloc, true);
    }
    
    static allocator create_proxy(const char* name = "proxy", allocator* backing = nullptr) {
        memento_allocator_t* backing_alloc = backing ? backing->allocator_ : get_root_allocator();
        memento_allocator_t* alloc = memento_create_proxy_allocator(name, backing_alloc);
        if (!alloc) {
            throw allocation_error(0, 0, name);
        }
        return allocator(alloc, true);
    }
    
    static allocator create_stack(const char* name = "stack", size_t capacity = 1024 * 1024, allocator* backing = nullptr) {
        memento_allocator_t* backing_alloc = backing ? backing->allocator_ : get_root_allocator();
        memento_allocator_t* alloc = memento_create_stack_allocator(name, capacity, backing_alloc);
        if (!alloc) {
            throw allocation_error(0, 0, name);
        }
        return allocator(alloc, true);
    }
    
    // Get underlying C allocator
    memento_allocator_t* get_internal_allocator() noexcept {
        return allocator_;
    }
    
    const memento_allocator_t* get_internal_allocator() const noexcept {
        return allocator_;
    }
    
private:
    static bool is_power_of_two(size_t value) noexcept {
        return (value & (value - 1)) == 0;
    }
    
    static memento_allocator_t* get_root_allocator() {
        /* This is a simplified version - in a real implementation, 
           we'd need access to the global root allocator */
        static memento_allocator_t* root = nullptr;
        if (!root) {
            root = memento_create_thread_cache("root_cpp");
        }
        return root;
    }
};

// Template arena for typed allocations
template<typename T>
class arena {
private:
    allocator allocator_;
    
public:
    explicit arena(allocator alloc) : allocator_(std::move(alloc)) {}
    
    explicit arena(const char* name = "arena", allocator* backing = nullptr) 
        : allocator_(backing ? allocator::create_proxy(name, backing) : allocator::create_proxy(name)) {}
    
    ~arena() = default;
    
    // Move semantics
    arena(arena&&) = default;
    arena& operator=(arena&&) = default;
    
    // No copy semantics
    arena(const arena&) = delete;
    arena& operator=(const arena&) = delete;
    
    // Allocation methods
    [[nodiscard]] T* allocate(size_t count = 1) {
        return allocator_.allocate_object<T>(count);
    }
    
    template<typename... Args>
    [[nodiscard]] T* make(Args&&... args) {
        return allocator_.construct<T>(std::forward<Args>(args)...);
    }
    
    void destroy(T* ptr) noexcept {
        allocator_.destroy(ptr);
    }
    
    void deallocate(T* ptr) noexcept {
        if (ptr) {
            allocator_.deallocate(ptr);
        }
    }
    
    // Statistics
    memento_stats_t stats() const {
        return allocator_.stats();
    }
    
    void print_stats() const {
        allocator_.print_stats();
    }
    
    const char* name() const noexcept {
        return allocator_.name();
    }
};

// STL-compatible allocator
template<typename T>
struct stl_allocator {
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    
private:
    allocator* allocator_;
    
public:
    explicit stl_allocator(allocator* alloc) : allocator_(alloc) {
        if (!allocator_) {
            throw std::invalid_argument("Null allocator pointer");
        }
    }
    
    stl_allocator() noexcept : allocator_(nullptr) {}
    
    // Copy constructor from different type
    template<typename U>
    stl_allocator(const stl_allocator<U>& other) noexcept : allocator_(other.get_allocator()) {}
    
    // Allocation
    [[nodiscard]] T* allocate(size_type n) {
        if (!allocator_) {
            throw std::runtime_error("STL allocator not properly initialized");
        }
        return static_cast<T*>(allocator_->allocate(n * sizeof(T), alignof(T)));
    }
    
    void deallocate(T* ptr, size_type) noexcept {
        if (allocator_) {
            allocator_->deallocate(ptr);
        }
    }
    
    size_type max_size() const noexcept {
        return std::numeric_limits<size_type>::max() / sizeof(T);
    }
    
    template<typename U, typename... Args>
    void construct(U* ptr, Args&&... args) {
        ::new(static_cast<void*>(ptr)) U(std::forward<Args>(args)...);
    }
    
    template<typename U>
    void destroy(U* ptr) {
        ptr->~U();
    }
    
    allocator* get_allocator() const noexcept {
        return allocator_;
    }
    
    // Comparison operators
    template<typename U>
    bool operator==(const stl_allocator<U>& other) const noexcept {
        return allocator_ == other.get_allocator();
    }
    
    template<typename U>
    bool operator!=(const stl_allocator<U>& other) const noexcept {
        return !(*this == other);
    }
};

// RAII unique_ptr implementation
template<typename T>
class unique_ptr {
private:
    T* ptr_;
    allocator* allocator_;
    
public:
    explicit unique_ptr(T* ptr = nullptr, allocator* alloc = nullptr) noexcept
        : ptr_(ptr), allocator_(alloc) {}
    
    ~unique_ptr() {
        reset();
    }
    
    // Move semantics
    unique_ptr(unique_ptr&& other) noexcept
        : ptr_(other.ptr_), allocator_(other.allocator_) {
        other.ptr_ = nullptr;
        other.allocator_ = nullptr;
    }
    
    unique_ptr& operator=(unique_ptr&& other) noexcept {
        if (this != &other) {
            reset();
            ptr_ = other.ptr_;
            allocator_ = other.allocator_;
            other.ptr_ = nullptr;
            other.allocator_ = nullptr;
        }
        return *this;
    }
    
    // No copy semantics
    unique_ptr(const unique_ptr&) = delete;
    unique_ptr& operator=(const unique_ptr&) = delete;
    
    // Pointer operations
    T& operator*() const {
        if (!ptr_) {
            throw std::runtime_error("Dereferencing null unique_ptr");
        }
        return *ptr_;
    }
    
    T* operator->() const {
        return ptr_;
    }
    
    T* get() const noexcept {
        return ptr_;
    }
    
    explicit operator bool() const noexcept {
        return ptr_ != nullptr;
    }
    
    void reset(T* new_ptr = nullptr) {
        if (ptr_ && allocator_) {
            allocator_->destroy(ptr_);
        }
        ptr_ = new_ptr;
        if (new_ptr) {
            /* Keep current allocator */
        } else {
            allocator_ = nullptr;
        }
    }
    
    T* release() noexcept {
        T* result = ptr_;
        ptr_ = nullptr;
        allocator_ = nullptr;
        return result;
    }
    
    void swap(unique_ptr& other) noexcept {
        std::swap(ptr_, other.ptr_);
        std::swap(allocator_, other.allocator_);
    }
    
    allocator* get_allocator() const noexcept {
        return allocator_;
    }
};

// Global initialization functions
inline bool initialize() {
    return memento_init() == MEMENTO_SUCCESS;
}

inline void shutdown() {
    memento_shutdown();
}

inline bool is_initialized() {
    return memento_is_initialized();
}

inline allocator get_root_allocator() {
    return allocator::create_thread_cache("root");
}

// Convenience macros
#define MEMENTO_OBJECT(allocator, Type) static_cast<Type*>((allocator).allocate_object<Type>())
#define MEMENTO_ARRAY(allocator, Type, Count) static_cast<Type*>((allocator).allocate_object<Type>(Count))
#define MEMENTO_BYTES(allocator, Size) (allocator).allocate(Size)
#define MEMENTO_ALIGNED_BYTES(allocator, Size, Alignment) (allocator).allocate(Size, Alignment)
#define MEMENTO_DEALLOC(allocator, Ptr) (allocator).deallocate(Ptr)

// Global new/delete overloads for types using memento
template<typename T>
struct global_new_delete {
    static void* operator new(size_t size) {
        if (!is_initialized()) {
            if (!initialize()) {
                throw initialization_error("Failed to initialize memento library");
            }
        }
        
        static thread_local allocator root_allocator = get_root_allocator();
        return root_allocator.allocate(size, alignof(T));
    }
    
    static void operator delete(void* ptr) noexcept {
        if (ptr && is_initialized()) {
            static thread_local allocator root_allocator = get_root_allocator();
            root_allocator.deallocate(ptr);
        }
    }
    
    static void* operator new[](size_t size) {
        return operator new(size);
    }
    
    static void operator delete[](void* ptr) noexcept {
        operator delete(ptr);
    }
    
    static void* operator new(size_t size, std::align_val_t alignment) {
        if (!is_initialized()) {
            if (!initialize()) {
                throw initialization_error("Failed to initialize memento library");
            }
        }
        
        static thread_local allocator root_allocator = get_root_allocator();
        return root_allocator.allocate(size, static_cast<size_t>(alignment));
    }
    
    static void operator delete(void* ptr, std::align_val_t) noexcept {
        operator delete(ptr);
    }
};

} // namespace memento

#endif /* MEMENTO_CPP_HPP */