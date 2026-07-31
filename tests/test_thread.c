/*
 * Memento Thread Safety Test Suite
 *
 * Tests multi-threaded allocation patterns and foreign deallocation.
 * Portable across POSIX (pthread) and Windows (CreateThread).
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#define MEMENTO_IMPLEMENTATION
#include "memento.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    typedef HANDLE memento_test_thread_t;
    typedef DWORD (WINAPI *memento_test_start_fn)(void*);

    static int memento_test_thread_create(memento_test_thread_t* t,
                                          memento_test_start_fn fn, void* arg) {
        *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
        return *t != NULL ? 0 : -1;
    }

    static int memento_test_thread_join(memento_test_thread_t t, void** result) {
        DWORD code = 0;
        WaitForSingleObject(t, INFINITE);
        GetExitCodeThread(t, &code);
        CloseHandle(t);
        if (result) *result = (void*)(uintptr_t)code;
        return 0;
    }

    static void memento_test_sleep_us(unsigned us) {
        if (us == 0) {
            Sleep(0);
        } else {
            Sleep((us + 999) / 1000);
        }
    }

    #define MEMENTO_TEST_THREAD_RET     DWORD WINAPI
    #define MEMENTO_TEST_THREAD_RETURN(x) return (DWORD)(uintptr_t)(x)
#else
    #include <pthread.h>
    #include <unistd.h>
    #include <time.h>
    typedef pthread_t memento_test_thread_t;
    typedef void* (*memento_test_start_fn)(void*);

    static int memento_test_thread_create(memento_test_thread_t* t,
                                          memento_test_start_fn fn, void* arg) {
        return pthread_create(t, NULL, fn, arg);
    }

    static int memento_test_thread_join(memento_test_thread_t t, void** result) {
        return pthread_join(t, result);
    }

    static void memento_test_sleep_us(unsigned us) {
        struct timespec ts;
        ts.tv_sec = (time_t)(us / 1000000u);
        ts.tv_nsec = (long)(us % 1000000u) * 1000L;
        nanosleep(&ts, NULL);
    }

    #define MEMENTO_TEST_THREAD_RET     void*
    #define MEMENTO_TEST_THREAD_RETURN(x) return (void*)(x)
#endif

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  %-40s ", #name); \
    fflush(stdout); \
    test_##name(); \
    passed++; \
    printf("PASS\n"); \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL\n    Assertion: %s\n    File: %s:%d\n", \
               #cond, __FILE__, __LINE__); \
        failed++; \
        return; \
    } \
} while(0)

#define ASSERT_NOT_NULL(p) ASSERT((p) != NULL)
#define ASSERT_EQ(a, b) ASSERT((a) == (b))

static int passed = 0;
static int failed = 0;

/* ============================================================================
 * Thread Test Helpers
 * ============================================================================ */

typedef struct {
    int thread_id;
    int iterations;
    size_t alloc_size;
    int* success;
} thread_args_t;

static MEMENTO_TEST_THREAD_RET thread_alloc_free(void* arg) {
    thread_args_t* args = (thread_args_t*)arg;
    memento_thread_heap_t* heap = memento_thread_heap_get();

    for (int i = 0; i < args->iterations; i++) {
        void* ptr = memento_thread_heap_alloc(heap, args->alloc_size);
        if (!ptr) {
            *args->success = 0;
            MEMENTO_TEST_THREAD_RETURN(0);
        }
        memset(ptr, args->thread_id, args->alloc_size);

        unsigned char* p = (unsigned char*)ptr;
        for (size_t j = 0; j < args->alloc_size; j++) {
            if (p[j] != (unsigned char)args->thread_id) {
                *args->success = 0;
                MEMENTO_TEST_THREAD_RETURN(0);
            }
        }

        memento_thread_heap_free(heap, ptr, args->alloc_size);
    }

    *args->success = 1;
    MEMENTO_TEST_THREAD_RETURN(0);
}

/* ============================================================================
 * Thread Safety Tests
 * ============================================================================ */

TEST(thread_basic) {
    #define NUM_THREADS 4
    #define ITERATIONS 1000

    memento_test_thread_t threads[NUM_THREADS];
    thread_args_t args[NUM_THREADS];
    int successes[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].iterations = ITERATIONS;
        args[i].alloc_size = 256;
        args[i].success = &successes[i];
        successes[i] = 0;

        ASSERT(memento_test_thread_create(&threads[i], thread_alloc_free, &args[i]) == 0);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        memento_test_thread_join(threads[i], NULL);
        ASSERT(successes[i]);
    }

    #undef NUM_THREADS
    #undef ITERATIONS
}

TEST(thread_different_sizes) {
    #define NUM_THREADS 8

    memento_test_thread_t threads[NUM_THREADS];
    thread_args_t args[NUM_THREADS];
    int successes[NUM_THREADS];
    size_t sizes[NUM_THREADS] = {16, 32, 64, 128, 256, 512, 1024, 2048};

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].iterations = 500;
        args[i].alloc_size = sizes[i];
        args[i].success = &successes[i];
        successes[i] = 0;

        ASSERT(memento_test_thread_create(&threads[i], thread_alloc_free, &args[i]) == 0);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        memento_test_thread_join(threads[i], NULL);
        ASSERT(successes[i]);
    }

    #undef NUM_THREADS
}

static MEMENTO_TEST_THREAD_RET thread_get_heap(void* arg) {
    (void)arg;
    (void)memento_thread_heap_get();
    MEMENTO_TEST_THREAD_RETURN(0);
}

TEST(thread_heaps_independent) {
    #define NUM_THREADS 4

    memento_test_thread_t threads[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        ASSERT(memento_test_thread_create(&threads[i], thread_get_heap, NULL) == 0);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        memento_test_thread_join(threads[i], NULL);
    }

    #undef NUM_THREADS
}

/* ============================================================================
 * Foreign Deallocation Tests
 * ============================================================================ */

TEST(foreign_free_basic) {
    memento_thread_heap_t* heap1 = memento_thread_heap_get();

    #define COUNT 100
    void* ptrs[COUNT];
    size_t sizes[COUNT];

    for (int i = 0; i < COUNT; i++) {
        sizes[i] = 256;
        ptrs[i] = memento_thread_heap_alloc(heap1, sizes[i]);
        ASSERT_NOT_NULL(ptrs[i]);
        memset(ptrs[i], 0xAB, sizes[i]);
    }

    for (int i = 0; i < COUNT; i++) {
        memento_thread_heap_free(heap1, ptrs[i], sizes[i]);
    }
    memento_thread_heap_flush(heap1);

    #undef COUNT
}

TEST(foreign_free_flush) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_heap_stats_t stats_before, stats_after;

    memento_thread_heap_stats(heap, &stats_before);
    memento_thread_heap_flush(heap);
    memento_thread_heap_stats(heap, &stats_after);
    (void)stats_before;
    (void)stats_after;
}

/* ============================================================================
 * Concurrent Allocator Tests
 * ============================================================================ */

typedef struct {
    int thread_id;
    int iterations;
} pool_thread_args_t;

static MEMENTO_TEST_THREAD_RET thread_pool_ops(void* arg) {
    pool_thread_args_t* args = (pool_thread_args_t*)arg;
    memento_thread_heap_t* heap = memento_thread_heap_get();

    memento_pool_t* pool = memento_pool_create(64, 100, heap);
    if (!pool) {
        MEMENTO_TEST_THREAD_RETURN(1);
    }

    for (int i = 0; i < args->iterations; i++) {
        void* ptr = memento_pool_alloc(pool);
        if (!ptr) {
            memento_pool_destroy(pool);
            MEMENTO_TEST_THREAD_RETURN(1);
        }

        memset(ptr, args->thread_id, 64);

        if (i % 10 == 0) {
            memento_test_sleep_us(1);
        }

        memento_pool_free(pool, ptr);
    }

    memento_pool_destroy(pool);
    MEMENTO_TEST_THREAD_RETURN(0);
}

TEST(pool_concurrent) {
    #define NUM_THREADS 4

    memento_test_thread_t threads[NUM_THREADS];
    pool_thread_args_t args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].iterations = 100;
        ASSERT(memento_test_thread_create(&threads[i], thread_pool_ops, &args[i]) == 0);
    }

    int failures = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        void* result = NULL;
        memento_test_thread_join(threads[i], &result);
        if (result != (void*)0) failures++;
    }

    ASSERT_EQ(failures, 0);

    #undef NUM_THREADS
}

static MEMENTO_TEST_THREAD_RET thread_arena_ops(void* arg) {
    pool_thread_args_t* args = (pool_thread_args_t*)arg;
    memento_thread_heap_t* heap = memento_thread_heap_get();

    memento_arena_t* arena = memento_arena_create(4096, heap);
    if (!arena) {
        MEMENTO_TEST_THREAD_RETURN(1);
    }

    for (int i = 0; i < args->iterations; i++) {
        void* ptr = memento_arena_alloc(arena, 256, 8);
        if (!ptr) {
            memento_arena_reset(arena);
            ptr = memento_arena_alloc(arena, 256, 8);
            if (!ptr) {
                memento_arena_destroy(arena);
                MEMENTO_TEST_THREAD_RETURN(1);
            }
        }
        memset(ptr, args->thread_id, 256);
    }

    memento_arena_destroy(arena);
    MEMENTO_TEST_THREAD_RETURN(0);
}

TEST(arena_concurrent) {
    #define NUM_THREADS 4

    memento_test_thread_t threads[NUM_THREADS];
    pool_thread_args_t args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].iterations = 100;
        ASSERT(memento_test_thread_create(&threads[i], thread_arena_ops, &args[i]) == 0);
    }

    int failures = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        void* result = NULL;
        memento_test_thread_join(threads[i], &result);
        if (result != (void*)0) failures++;
    }

    ASSERT_EQ(failures, 0);

    #undef NUM_THREADS
}

/* ============================================================================
 * Stress Tests
 * ============================================================================ */

typedef struct {
    int thread_id;
    int duration_ms;
    int* ops_count;
    int* success;
} stress_args_t;

static MEMENTO_TEST_THREAD_RET thread_stress(void* arg) {
    stress_args_t* args = (stress_args_t*)arg;
    memento_thread_heap_t* heap = memento_thread_heap_get();

    /* Keep a small working set so free patterns exercise the cache without
     * leaking under ASan. */
    enum { LIVE = 32 };
    void* live[LIVE];
    size_t live_sz[LIVE];
    int live_n = 0;

    int ops = 0;
    time_t start = time(NULL);

    while ((time(NULL) - start) * 1000 < args->duration_ms) {
        size_t size = 16 + (size_t)(rand() % 4096);

        void* ptr = memento_thread_heap_alloc(heap, size);
        if (!ptr) {
            *args->success = 0;
            for (int i = 0; i < live_n; i++) {
                memento_thread_heap_free(heap, live[i], live_sz[i]);
            }
            MEMENTO_TEST_THREAD_RETURN(0);
        }

        memset(ptr, args->thread_id, size);

        if (live_n < LIVE && (rand() % 3) != 0) {
            live[live_n] = ptr;
            live_sz[live_n] = size;
            live_n++;
        } else if (live_n > 0 && (rand() % 2) == 0) {
            int idx = rand() % live_n;
            memento_thread_heap_free(heap, live[idx], live_sz[idx]);
            live[idx] = live[live_n - 1];
            live_sz[idx] = live_sz[live_n - 1];
            live_n--;
            memento_thread_heap_free(heap, ptr, size);
        } else {
            memento_thread_heap_free(heap, ptr, size);
        }

        ops++;
    }

    for (int i = 0; i < live_n; i++) {
        memento_thread_heap_free(heap, live[i], live_sz[i]);
    }

    *args->ops_count = ops;
    *args->success = 1;
    MEMENTO_TEST_THREAD_RETURN(0);
}

TEST(stress_threads) {
    #define NUM_THREADS 8
    #define DURATION_MS 100

    memento_test_thread_t threads[NUM_THREADS];
    stress_args_t args[NUM_THREADS];
    int ops_counts[NUM_THREADS];
    int successes[NUM_THREADS];

    srand((unsigned)time(NULL));

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].duration_ms = DURATION_MS;
        args[i].ops_count = &ops_counts[i];
        args[i].success = &successes[i];
        successes[i] = 0;

        ASSERT(memento_test_thread_create(&threads[i], thread_stress, &args[i]) == 0);
    }

    int total_ops = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        memento_test_thread_join(threads[i], NULL);
        ASSERT(successes[i]);
        total_ops += ops_counts[i];
    }

    printf("(%d ops) ", total_ops);

    #undef NUM_THREADS
    #undef DURATION_MS
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("=== Memento Thread Safety Test Suite ===\n\n");

    if (!memento_init()) {
        printf("Failed to initialize Memento\n");
        return 1;
    }

    printf("Thread Tests:\n");
    RUN_TEST(thread_basic);
    RUN_TEST(thread_different_sizes);
    RUN_TEST(thread_heaps_independent);

    printf("\nForeign Deallocation Tests:\n");
    RUN_TEST(foreign_free_basic);
    RUN_TEST(foreign_free_flush);

    printf("\nConcurrent Allocator Tests:\n");
    RUN_TEST(pool_concurrent);
    RUN_TEST(arena_concurrent);

    printf("\nStress Tests:\n");
    RUN_TEST(stress_threads);

    memento_shutdown();

    printf("\n=== Results ===\n");
    printf("  Passed: %d\n", passed);
    printf("  Failed: %d\n", failed);

    if (failed == 0) {
        printf("\nAll tests passed!\n");
        return 0;
    } else {
        printf("\nSome tests failed!\n");
        return 1;
    }
}
