/*
 * Memento C++ Wrapper
 *
 * A modern C++17/20 wrapper for the Memento memory allocator library.
 * Non-locking, header-only design.
 *
 * Usage:
 *   #include "memento.hpp"
 *
 *   int main() {
 *       memento::context ctx;           // RAII init
 *       memento::heap h;                // Thread-local heap
 *       auto obj = h.construct<T>(...); // Allocate + construct
 *       h.destroy(obj);                 // Destroy + deallocate
 *       return 0;
 *   }
 *
 * License: MIT
 */

#ifndef MEMENTO_HPP
#define MEMENTO_HPP

#include <type_traits>
#include <utility>
#include <memory>
#include <vector>
#include <cstddef>
#include <limits>
#include <iostream>
#include <new>
#include <stdexcept>

/* Feature detection */
#if __cplusplus >= 202002L || (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L)
    #ifndef MEMENTO_CPP20
        #define MEMENTO_CPP20 1
    #endif
    #ifndef MEMENTO_CPP17
        #define MEMENTO_CPP17 1
    #endif
#elif __cplusplus >= 201703L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L)
    #ifndef MEMENTO_CPP20
        #define MEMENTO_CPP20 0
    #endif
    #ifndef MEMENTO_CPP17
        #define MEMENTO_CPP17 1
    #endif
#else
    #error "C++17 or later required for Memento C++ wrapper"
#endif

#if MEMENTO_CPP20
#include <concepts>
#endif

/* memento.h already wraps its public API in extern "C". Do NOT wrap the
 * include itself — that would pull <atomic> under C linkage. */
#include "memento.h"

#ifndef MEMENTO_VERSION
    #error "Memento C header version macros not found"
#endif

#if MEMENTO_VERSION < 0x020000
    #error "Memento C++ wrapper requires C header version 2.0.0 or later"
#endif

/* Force-inline for STL allocator hot path */
#if defined(_MSC_VER) && !defined(__clang__)
    #define MEMENTO_CPP_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
    #define MEMENTO_CPP_FORCE_INLINE __attribute__((always_inline)) inline
#else
    #define MEMENTO_CPP_FORCE_INLINE inline
#endif

namespace memento {

#if MEMENTO_CPP20
template<typename T>
concept Allocatable = std::is_nothrow_destructible_v<T>;

#define MEMENTO_REQUIRES_ALLOCATABLE requires Allocatable<T>
#define MEMENTO_REQUIRES_ALLOCATABLE_U requires Allocatable<U>
#else
#define MEMENTO_REQUIRES_ALLOCATABLE
#define MEMENTO_REQUIRES_ALLOCATABLE_U
#endif

/* ============================================================================
 * RAII Context
 * ============================================================================ */

class context {
public:
    context() {
        if (!memento_init()) {
            throw std::runtime_error("Failed to initialize Memento");
        }
    }

    ~context() {
        memento_shutdown();
    }

    context(const context&) = delete;
    context& operator=(const context&) = delete;
    context(context&&) = delete;
    context& operator=(context&&) = delete;
};

/* ============================================================================
 * Heap Wrapper
 * ============================================================================ */

class heap {
public:
    heap() : handle_(memento_thread_heap_get()) {
        if (!handle_) {
            throw std::bad_alloc();
        }
    }

    explicit heap(memento_thread_heap_t* h) : handle_(h) {
        if (!handle_) {
            throw std::bad_alloc();
        }
    }

    [[nodiscard]] void* allocate(size_t size) {
        void* ptr = memento_thread_heap_alloc(handle_, size);
        if (!ptr && size > 0) {
            throw std::bad_alloc();
        }
        return ptr;
    }

    void deallocate(void* ptr, size_t size) {
        memento_thread_heap_free(handle_, ptr, size);
    }

    template<typename T, typename... Args>
    MEMENTO_REQUIRES_ALLOCATABLE
    [[nodiscard]] T* construct(Args&&... args) {
        void* ptr = allocate(sizeof(T));
        try {
            return new (ptr) T(std::forward<Args>(args)...);
        } catch (...) {
            deallocate(ptr, sizeof(T));
            throw;
        }
    }

    template<typename T>
    MEMENTO_REQUIRES_ALLOCATABLE
    void destroy(T* ptr) {
        if (ptr) {
            ptr->~T();
            deallocate(ptr, sizeof(T));
        }
    }

    /* Raw array storage (does not construct). Pair with construct loops or
     * use only for trivially constructible types. */
    template<typename T>
    MEMENTO_REQUIRES_ALLOCATABLE
    [[nodiscard]] T* allocate_array(size_t count) {
        if (count > 0 && count > std::numeric_limits<size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length();
        }
        return static_cast<T*>(allocate(sizeof(T) * count));
    }

    template<typename T>
    MEMENTO_REQUIRES_ALLOCATABLE
    void deallocate_array(T* ptr, size_t count) {
        if (ptr) {
            for (size_t i = 0; i < count; i++) {
                ptr[i].~T();
            }
            deallocate(ptr, sizeof(T) * count);
        }
    }

    void flush() {
        memento_thread_heap_flush(handle_);
    }

    memento_thread_heap_t* handle() const { return handle_; }

private:
    memento_thread_heap_t* handle_;
};

/* ============================================================================
 * Scoped Pointer
 * ============================================================================ */

template<typename T>
class scoped_ptr {
public:
    explicit scoped_ptr(T* ptr = nullptr, heap* h = nullptr)
        : ptr_(ptr), heap_(h) {}

    ~scoped_ptr() {
        reset();
    }

    scoped_ptr(const scoped_ptr&) = delete;
    scoped_ptr& operator=(const scoped_ptr&) = delete;

    scoped_ptr(scoped_ptr&& other) noexcept
        : ptr_(other.ptr_), heap_(other.heap_) {
        other.ptr_ = nullptr;
        other.heap_ = nullptr;
    }

    scoped_ptr& operator=(scoped_ptr&& other) noexcept {
        if (this != &other) {
            reset();
            ptr_ = other.ptr_;
            heap_ = other.heap_;
            other.ptr_ = nullptr;
            other.heap_ = nullptr;
        }
        return *this;
    }

    T& operator*() const { return *ptr_; }
    T* operator->() const { return ptr_; }
    T* get() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    void reset(T* ptr = nullptr) {
        if (ptr_) {
            if (heap_) {
                heap_->destroy(ptr_);
            } else {
                delete ptr_;
            }
        }
        ptr_ = ptr;
    }

    T* release() {
        T* tmp = ptr_;
        ptr_ = nullptr;
        return tmp;
    }

private:
    T* ptr_;
    heap* heap_;
};

/* ============================================================================
 * Pool Wrapper
 * ============================================================================ */

template<typename T>
class pool {
public:
    explicit pool(size_t capacity, heap* h = nullptr)
        : heap_(h ? h->handle() : memento_thread_heap_get())
        , handle_(memento_pool_create(sizeof(T), capacity, heap_)) {
        if (!handle_) {
            throw std::bad_alloc();
        }
    }

    ~pool() {
        if (handle_) {
            memento_pool_destroy(handle_);
        }
    }

    pool(const pool&) = delete;
    pool& operator=(const pool&) = delete;

    pool(pool&& other) noexcept
        : heap_(other.heap_), handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    pool& operator=(pool&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                memento_pool_destroy(handle_);
            }
            heap_ = other.heap_;
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    template<typename... Args>
    [[nodiscard]] T* emplace(Args&&... args) {
        void* ptr = memento_pool_alloc(handle_);
        if (!ptr) {
            throw std::bad_alloc();
        }
        try {
            return new (ptr) T(std::forward<Args>(args)...);
        } catch (...) {
            memento_pool_free(handle_, ptr);
            throw;
        }
    }

    void destroy(T* ptr) {
        if (ptr) {
            ptr->~T();
            memento_pool_free(handle_, ptr);
        }
    }

    [[nodiscard]] T* allocate() {
        return static_cast<T*>(memento_pool_alloc(handle_));
    }

    void deallocate(T* ptr) {
        memento_pool_free(handle_, ptr);
    }

    memento_pool_t* handle() const { return handle_; }

private:
    memento_thread_heap_t* heap_;
    memento_pool_t* handle_;
};

/* ============================================================================
 * Arena Wrapper
 * ============================================================================ */

class arena {
public:
    explicit arena(size_t initial_capacity, heap* h = nullptr)
        : heap_(h ? h->handle() : memento_thread_heap_get())
        , handle_(memento_arena_create(initial_capacity, heap_)) {
        if (!handle_) {
            throw std::bad_alloc();
        }
    }

    ~arena() {
        if (handle_) {
            memento_arena_destroy(handle_);
        }
    }

    arena(const arena&) = delete;
    arena& operator=(const arena&) = delete;

    arena(arena&& other) noexcept
        : heap_(other.heap_), handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    arena& operator=(arena&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                memento_arena_destroy(handle_);
            }
            heap_ = other.heap_;
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] void* allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        void* ptr = memento_arena_alloc(handle_, size, alignment);
        if (!ptr && size > 0) {
            throw std::bad_alloc();
        }
        return ptr;
    }

    template<typename T, typename... Args>
    [[nodiscard]] T* construct(Args&&... args) {
        void* ptr = allocate(sizeof(T), alignof(T));
        try {
            return new (ptr) T(std::forward<Args>(args)...);
        } catch (...) {
            /* Arena cannot free a single object; leave the bump as-is.
             * The save/restore or reset API is the intended cleanup path. */
            throw;
        }
    }

    class save_point {
    public:
        memento_arena_save_t state;
        friend class arena;
    };

    [[nodiscard]] save_point save() {
        save_point sp;
        sp.state = memento_arena_save(handle_);
        return sp;
    }

    void restore(const save_point& sp) {
        memento_arena_restore(handle_, const_cast<memento_arena_save_t*>(&sp.state));
    }

    void reset() {
        memento_arena_reset(handle_);
    }

    [[nodiscard]] size_t used() const {
        return memento_arena_used(handle_);
    }

    [[nodiscard]] size_t capacity() const {
        return memento_arena_capacity(handle_);
    }

    memento_arena_t* handle() const { return handle_; }

private:
    memento_thread_heap_t* heap_;
    memento_arena_t* handle_;
};

/* ============================================================================
 * Stack Wrapper
 * ============================================================================ */

template<typename T>
class stack {
public:
    explicit stack(size_t capacity, heap* h = nullptr)
        : heap_(h ? h->handle() : memento_thread_heap_get())
        , handle_(memento_stack_create(capacity * sizeof(T), heap_)) {
        if (!handle_) {
            throw std::bad_alloc();
        }
    }

    ~stack() {
        if (handle_) {
            memento_stack_destroy(handle_);
        }
    }

    stack(const stack&) = delete;
    stack& operator=(const stack&) = delete;

    stack(stack&& other) noexcept
        : heap_(other.heap_), handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    stack& operator=(stack&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                memento_stack_destroy(handle_);
            }
            heap_ = other.heap_;
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    template<typename... Args>
    [[nodiscard]] T* push(Args&&... args) {
        void* ptr = memento_stack_push(handle_, sizeof(T), alignof(T));
        if (!ptr) {
            throw std::bad_alloc();
        }
        try {
            return new (ptr) T(std::forward<Args>(args)...);
        } catch (...) {
            memento_stack_pop(handle_, ptr);
            throw;
        }
    }

    void pop(T* ptr) {
        if (ptr) {
            ptr->~T();
            memento_stack_pop(handle_, ptr);
        }
    }

    class marker {
    public:
        memento_stack_marker_t state;
        friend class stack;
    };

    [[nodiscard]] marker mark() {
        marker m;
        m.state = memento_stack_marker(handle_);
        return m;
    }

    void restore(const marker& m) {
        memento_stack_pop_to_marker(handle_, m.state);
    }

    void reset() {
        memento_stack_reset(handle_);
    }

    memento_stack_t* handle() const { return handle_; }

private:
    memento_thread_heap_t* heap_;
    memento_stack_t* handle_;
};

/* ============================================================================
 * STL Allocator
 * ============================================================================ */

template<typename T>
class allocator {
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_copy_assignment = std::false_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;

    allocator() noexcept : heap_(memento_thread_heap_get()) {}

    explicit allocator(heap& h) noexcept : heap_(h.handle()) {}

    explicit allocator(memento_thread_heap_t* h) noexcept : heap_(h) {}

    template<typename U>
    allocator(const allocator<U>& other) noexcept : heap_(other.heap_) {}

    [[nodiscard]] MEMENTO_CPP_FORCE_INLINE T* allocate(std::size_t n) {
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length();
        }
        void* ptr = memento_thread_heap_alloc(heap_, n * sizeof(T));
        if (!ptr) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(ptr);
    }

    MEMENTO_CPP_FORCE_INLINE void deallocate(T* ptr, std::size_t n) noexcept {
        memento_thread_heap_free(heap_, ptr, n * sizeof(T));
    }

    template<typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }

    template<typename U>
    void destroy(U* p) {
        p->~U();
    }

    template<typename U>
    struct rebind {
        using other = allocator<U>;
    };

    allocator select_on_container_copy_construction() const {
        return *this;
    }

    bool operator==(const allocator& other) const noexcept {
        return heap_ == other.heap_;
    }

    bool operator!=(const allocator& other) const noexcept {
        return !(*this == other);
    }

    memento_thread_heap_t* get_heap() const noexcept { return heap_; }

private:
    template<typename U>
    friend class allocator;

    memento_thread_heap_t* heap_;
};

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

inline void print_stats(std::ostream& os = std::cout) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_heap_stats_t stats;
    memento_thread_heap_stats(heap, &stats);

    os << "Memento Stats:\n";
    os << "  Allocations: " << stats.alloc_count << "\n";
    os << "  Deallocations: " << stats.free_count << "\n";
    os << "  Bytes allocated: " << stats.bytes_allocated << "\n";
    os << "  Bytes freed: " << stats.bytes_freed << "\n";
    os << "  Foreign frees: " << stats.foreign_free_count << "\n";
}

} // namespace memento

#endif /* MEMENTO_HPP */
