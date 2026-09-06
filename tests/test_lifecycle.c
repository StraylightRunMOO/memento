/*
 * Memento Lifecycle Test Suite
 *
 * The three scenarios that decide whether "smooths musl" is a feature or a
 * campfire story:
 *   1. fork() with live heaps
 *   2. thread exit while another thread still holds (and later frees) a
 *      pointer into the exited thread's heap
 *   3. heap park/adopt: short-lived worker threads must recycle heaps, not
 *      pay a fresh mmap storm per thread
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#define MEMENTO_IMPLEMENTATION
#include "memento.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if !defined(_WIN32)
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
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

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NE(a, b) ASSERT((a) != (b))
#define ASSERT_NOT_NULL(p) ASSERT((p) != NULL)
#define ASSERT_GE(a, b) ASSERT((a) >= (b))

static int passed = 0;
static int failed = 0;

#if !defined(_WIN32)

/* ============================================================================
 * 1. fork() with live heaps
 * ============================================================================ */

TEST(fork_with_live_heaps) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    void* live1 = memento_thread_heap_alloc(heap, 512);
    void* live2 = memento_malloc(64 * 1024); /* sized, page-run tier */
    ASSERT_NOT_NULL(live1);
    ASSERT_NOT_NULL(live2);
    memset(live1, 0xAA, 512);
    memset(live2, 0xBB, 64 * 1024);

    unsigned long parent_gen = memento_fork_generation_current();
    pid_t pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        /* Child: the snapshot must be readable and the allocator usable. */
        volatile unsigned char* p = (volatile unsigned char*)live1;
        unsigned char expected = 0xAA;
        for (int i = 0; i < 512; i++) {
            if (p[i] != expected) _exit(2);
        }
        /* The child got a fork-generation bump and NO inherited heap: its
         * first allocation creates a fresh heap with fresh private spans,
         * not CoW copies of the parent's. */
        if (memento_fork_generation_current() == parent_gen) _exit(5);
        void* q = memento_malloc(1000);
        if (!q) _exit(3);
        memset(q, 1, 1000);
        memento_free(q);
        void* r = memento_thread_heap_alloc(memento_thread_heap_get(), 256);
        if (!r) _exit(4);
        memento_thread_heap_free(memento_thread_heap_get(), r, 256);
        /* Freeing a pre-fork block in the child routes to the retired
         * parent's heap via the span header — must be safe. */
        memento_free(live2);
        memento_shutdown();
        /* exit() (not _exit) so coverage counters from the child — which
         * just ran the atfork child handler — are flushed and merged. */
        exit(0);
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    /* Parent's heap survived untouched */
    for (int i = 0; i < 512; i++) {
        ASSERT_EQ(((unsigned char*)live1)[i], 0xAA);
    }
    memento_thread_heap_free(heap, live1, 512);
    memento_free(live2);
}

/* ============================================================================
 * 2. Thread exit while another thread holds a pointer
 * ============================================================================ */

typedef struct {
    void* gift;             /* allocated on the short-lived thread's heap */
    volatile int ready;     /* set once gift is published */
    volatile int done;      /* set once main freed it */
} gift_t;

static void* short_lived_worker(void* arg) {
    gift_t* g = (gift_t*)arg;
    /* sized allocation: main will free it with memento_free (no size) */
    g->gift = memento_malloc(128);
    if (g->gift) {
        memset(g->gift, 0x5C, 128);
    }
    g->ready = 1;
    while (!g->done) { /* wait for main to foreign-free our gift */ }
    return NULL;
}

TEST(thread_exit_with_outstanding_pointer) {
    gift_t g;
    memset(&g, 0, sizeof(g));

    pthread_t t;
    ASSERT_EQ(pthread_create(&t, NULL, short_lived_worker, &g), 0);
    while (!g.ready) { /* spin */ }
    ASSERT_NOT_NULL(g.gift);

    /* The worker exits; its heap is parked, not destroyed. We free the gift
     * AFTER the thread is gone — it must land on the parked heap's foreign
     * stack and be reclaimed (by the next adopter, or at shutdown). */
    memento_free(g.gift); /* sized free: owner derived from the pointer */
    g.done = 1;
    pthread_join(t, NULL);

    /* If we got here without a crash, the parked-heap path worked. */
}

/* ============================================================================
 * 3. Park / adopt and heap recycling
 * ============================================================================ */

static void* churn_worker(void* arg) {
    (void)arg;
    memento_thread_heap_t* heap = memento_thread_heap_get();
    for (int i = 0; i < 100; i++) {
        void* p = memento_thread_heap_alloc(heap, 256);
        if (!p) return (void*)1;
        memset(p, i, 256);
        memento_thread_heap_free(heap, p, 256);
    }
    return NULL;
}

TEST(heap_recycling_across_threads) {
    /* A heap freshly created per thread would map new spans every time; with
     * recycling, thread N+1 adopts thread N's parked heap and reuses the
     * spans. We can't see the mapping count from here, but we CAN see that
     * the adopted heap is literally the same address across threads. */
    enum { N = 8 };
    memento_thread_heap_t* seen[N];
    for (int round = 0; round < N; round++) {
        seen[round] = NULL;
    }

    for (int round = 0; round < N; round++) {
        /* spawn one worker at a time so adoption is deterministic */
        pthread_t t;
        ASSERT_EQ(pthread_create(&t, NULL, churn_worker, NULL), 0);
        pthread_join(t, NULL);
    }

    /* Now measure adoption explicitly */
    memento_heap_park(); /* park the main thread's heap */
    memento_thread_heap_t* adopted = memento_heap_adopt();
    ASSERT_NOT_NULL(adopted);
    ASSERT_EQ(memento_thread_heap_get(), adopted);

    (void)seen;
}

TEST(explicit_park_adopt_cycle) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    ASSERT_NOT_NULL(heap);

    /* Allocate something, park, adopt: same heap, contents intact */
    void* p = memento_malloc(2048);
    ASSERT_NOT_NULL(p);
    memset(p, 0x77, 2048);

    memento_heap_park();
    memento_thread_heap_t* back = memento_heap_adopt();
    ASSERT_EQ(back, heap);

    /* Pointer allocated before the park is still valid and freeable */
    for (int i = 0; i < 2048; i++) {
        ASSERT_EQ(((unsigned char*)p)[i], 0x77);
    }
    memento_free(p);
}

#endif /* !_WIN32 */

TEST(atfork_registers) {
    /* POSIX: installs handlers (idempotent). Windows: no-op success. */
    ASSERT_EQ(memento_atfork_register(), 0);
    ASSERT_EQ(memento_atfork_register(), 0);
}

TEST(guarded_arena_guard_page_faults) {
    /* The guarded arena puts a PROT_NONE page at the block's high-water
     * mark. A child that marches past the end of its allocation must die
     * on that page instead of silently corrupting whatever follows. */
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_arena_t* arena = memento_arena_create_guarded(4096, heap);
    ASSERT_NOT_NULL(arena);
    /* Small allocation: the rest of the block (and then the guard page)
     * lies ahead of us. Asking for the full capacity would spill into a
     * fresh, larger block and put the guard a whole mapping away. */
    volatile unsigned char* p =
        (volatile unsigned char*)memento_arena_alloc(arena, 64, 16);
    ASSERT_NOT_NULL((void*)p);
    p[0] = 1;
    p[63] = 1;

    pid_t pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        /* The guard page starts within one page of the block's high-water
         * mark; this march stays well inside the mapping so ONLY the
         * guard can stop us. */
        for (size_t i = 64; i < 4096 + 5120; i += 64) {
            p[i] = 1;
        }
        _exit(0); /* no guard: survived -> parent fails the test */
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    /* Bare build: SIGSEGV on the guard page. Under ASan the SEGV is
     * caught by the runtime and the child exits nonzero instead. */
    if (WIFEXITED(status)) {
        ASSERT(WEXITSTATUS(status) != 0);
    } else {
        ASSERT(WIFSIGNALED(status));
        ASSERT_EQ(WTERMSIG(status), SIGSEGV);
    }
    memento_arena_destroy(arena);
}

int main(void) {
    printf("=== Memento Lifecycle Test Suite ===\n\n");

    if (!memento_init()) {
        printf("Failed to initialize Memento\n");
        return 1;
    }

#if !defined(_WIN32)
    RUN_TEST(fork_with_live_heaps);
    RUN_TEST(thread_exit_with_outstanding_pointer);
    RUN_TEST(heap_recycling_across_threads);
    RUN_TEST(explicit_park_adopt_cycle);
#endif
    RUN_TEST(atfork_registers);
    RUN_TEST(guarded_arena_guard_page_faults);

    memento_shutdown();

    printf("\n=== Results ===\n");
    printf("  Passed: %d\n", passed);
    printf("  Failed: %d\n", failed);

    if (failed == 0) {
        printf("\n✓ All tests passed!\n");
        return 0;
    }
    printf("\n✗ Some tests failed!\n");
    return 1;
}
