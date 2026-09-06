/*
 * Memento Proxy Test Suite
 *
 * Compiled with -DMEMENTO_PROXY_DEBUG=1: proxies track every live allocation
 * with a cookie, owner thread id, size, and call site, and report leaks
 * grouped by site. The release-mode proxy (compiled out) is exercised by
 * test_proxy_release.c ... just kidding, same file, CMake builds it twice
 * with different defines.
 */

#define MEMENTO_IMPLEMENTATION
#include "memento.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

TEST(site_id_stable) {
    uint64_t a = memento_site_id("foo.c", 42);
    uint64_t b = memento_site_id("foo.c", 42);
    uint64_t c = memento_site_id("foo.c", 43);
    uint64_t d = memento_site_id("bar.c", 42);
    ASSERT_EQ(a, b);
    ASSERT_NE(a, c);
    ASSERT_NE(a, d);
}

TEST(proxy_heap_roundtrip) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_proxy_t* px = memento_proxy_wrap_heap(heap, MEMENTO_PROXY_DEBUG_ALL);
    ASSERT_NOT_NULL(px);
    ASSERT_NOT_NULL(px->alloc);
    ASSERT_NOT_NULL(px->free_);

    void* p1 = memento_proxy_alloc(px, 256);
    void* p2 = memento_proxy_alloc_aligned(px, 1000, 64);
    ASSERT_NOT_NULL(p1);
    ASSERT_NOT_NULL(p2);
    ASSERT(((uintptr_t)p2 & 63) == 0);
    memset(p1, 1, 256);
    memset(p2, 2, 1000);

#if MEMENTO_PROXY_DEBUG
    ASSERT_EQ(memento_proxy_outstanding_count(px), 2);
    ASSERT_EQ(memento_proxy_outstanding_bytes(px), 256 + 1000);
#endif

    /* vtable call form also works */
    void* p3 = px->alloc(px->impl, 128, 0);
    ASSERT_NOT_NULL(p3);

    memento_proxy_free(px, p1);
    memento_proxy_free(px, p2);
    px->free_(px->impl, p3, 128);

#if MEMENTO_PROXY_DEBUG
    ASSERT_EQ(memento_proxy_outstanding_count(px), 0);
    ASSERT_EQ(memento_proxy_outstanding_bytes(px), 0);
#endif
    memento_proxy_destroy(px);
}

TEST(proxy_arena_leak_report) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_arena_t* arena = memento_arena_create(64 * 1024, heap);
    ASSERT_NOT_NULL(arena);
    memento_proxy_t* px = memento_proxy_wrap_arena(arena, MEMENTO_PROXY_DEBUG_ALL);
    ASSERT_NOT_NULL(px);

    /* "leak" two allocations, free one properly */
    void* live1 = memento_proxy_alloc(px, 512);
    void* live2 = memento_proxy_alloc(px, 1024);
    void* gone = memento_proxy_alloc(px, 128);
    ASSERT_NOT_NULL(live1);
    ASSERT_NOT_NULL(live2);
    ASSERT_NOT_NULL(gone);
    memento_proxy_free(px, gone); /* untracks; arena memory reclaimed at reset */

#if MEMENTO_PROXY_DEBUG
    ASSERT_EQ(memento_proxy_outstanding_count(px), 2);
    ASSERT_EQ(memento_proxy_outstanding_bytes(px), 512 + 1024);

    /* The report must name this file and both live sites */
    FILE* f = tmpfile();
    ASSERT_NOT_NULL(f);
    memento_proxy_report(px, f);
    fflush(f);
    fseek(f, 0, SEEK_SET);
    static char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);

    ASSERT(strstr(buf, "proxy report (arena") != NULL);
    ASSERT(strstr(buf, "outstanding 2 blocks, 1536 bytes") != NULL);
    ASSERT(strstr(buf, "test_proxy.c") != NULL);
#endif

    memento_arena_reset(arena);
    memento_proxy_destroy(px);
    memento_arena_destroy(arena);
}

TEST(proxy_pool_and_slab) {
    memento_thread_heap_t* heap = memento_thread_heap_get();

    memento_pool_t* pool = memento_pool_create(96, 32, heap);
    ASSERT_NOT_NULL(pool);
    memento_proxy_t* pp = memento_proxy_wrap_pool(pool, MEMENTO_PROXY_DEBUG_ALL);
    ASSERT_NOT_NULL(pp);
    void* o1 = memento_proxy_alloc(pp, 96);
    void* o2 = memento_proxy_alloc(pp, 96);
    ASSERT_NOT_NULL(o1);
    ASSERT_NOT_NULL(o2);
    memento_proxy_free(pp, o1);
#if MEMENTO_PROXY_DEBUG
    ASSERT_EQ(memento_proxy_outstanding_count(pp), 1);
#endif
    memento_proxy_free(pp, o2);
    memento_proxy_destroy(pp);
    memento_pool_destroy(pool);

    memento_slab_t* slab = memento_slab_create(heap);
    ASSERT_NOT_NULL(slab);
    memento_proxy_t* ps = memento_proxy_wrap_slab(slab, MEMENTO_PROXY_DEBUG_ALL);
    ASSERT_NOT_NULL(ps);
    void* s1 = memento_proxy_alloc(ps, 300);
    ASSERT_NOT_NULL(s1);
    memento_proxy_free(ps, s1);
#if MEMENTO_PROXY_DEBUG
    ASSERT_EQ(memento_proxy_outstanding_count(ps), 0);
#endif
    memento_proxy_destroy(ps);
    memento_slab_destroy(slab);
}

TEST(proxy_stack_lifo) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_stack_t* stack = memento_stack_create(64 * 1024, heap);
    ASSERT_NOT_NULL(stack);
    memento_proxy_t* px = memento_proxy_wrap_stack(stack, MEMENTO_PROXY_DEBUG_ALL);
    ASSERT_NOT_NULL(px);

    void* a = memento_proxy_alloc(px, 100);
    void* b = memento_proxy_alloc(px, 200);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    /* stack pops are LIFO */
    memento_proxy_free(px, b);
    memento_proxy_free(px, a);
#if MEMENTO_PROXY_DEBUG
    ASSERT_EQ(memento_proxy_outstanding_count(px), 0);
#endif
    memento_proxy_destroy(px);
    memento_stack_destroy(stack);
}

TEST(proxy_clean_report) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_proxy_t* px = memento_proxy_wrap_heap(heap, MEMENTO_PROXY_DEBUG_ALL);
    ASSERT_NOT_NULL(px);
    void* p = memento_proxy_alloc(px, 64);
    memento_proxy_free(px, p);

#if MEMENTO_PROXY_DEBUG
    FILE* f = tmpfile();
    ASSERT_NOT_NULL(f);
    memento_proxy_report(px, f);
    fflush(f);
    fseek(f, 0, SEEK_SET);
    static char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    ASSERT(strstr(buf, "no outstanding allocations") != NULL);
#endif
    memento_proxy_destroy(px);
}

TEST(proxy_shows_up_in_heap_report) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_proxy_t* px = memento_proxy_wrap_heap(heap, MEMENTO_PROXY_DEBUG_ALL);
    ASSERT_NOT_NULL(px);
    void* p = memento_proxy_alloc(px, 2048);
    ASSERT_NOT_NULL(p);

    FILE* f = tmpfile();
    ASSERT_NOT_NULL(f);
    memento_thread_heap_report(heap, f);
    fflush(f);
    fseek(f, 0, SEEK_SET);
    static char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    ASSERT(strstr(buf, "proxies:") != NULL);

    memento_proxy_free(px, p);
    memento_proxy_destroy(px);
}

TEST(proxy_null_safety) {
    memento_proxy_free(NULL, NULL);
    memento_proxy_destroy(NULL);
    ASSERT_EQ(memento_proxy_outstanding_count(NULL), 0);
    ASSERT_EQ(memento_proxy_outstanding_bytes(NULL), 0);
    memento_proxy_report(NULL, NULL);
    ASSERT(memento_proxy_wrap_heap(NULL, 0) == NULL);
}

TEST(proxy_table_grows) {
    /* Enough live blocks to force the side table through several doublings
     * (initial cap 64, grows at 70% load). */
    memento_thread_heap_t* heap = memento_thread_heap_get();
    memento_proxy_t* px = memento_proxy_wrap_heap(heap, MEMENTO_PROXY_DEBUG_ALL);
    ASSERT_NOT_NULL(px);

    enum { N = 512 };
    void* ptrs[N];
    size_t total = 0;
    for (int i = 0; i < N; i++) {
        size_t sz = 24 + (size_t)(i % 97);
        ptrs[i] = memento_proxy_alloc(px, sz);
        ASSERT_NOT_NULL(ptrs[i]);
        memset(ptrs[i], i, sz);
        total += sz;
    }
#if MEMENTO_PROXY_DEBUG
    ASSERT_EQ(memento_proxy_outstanding_count(px), (size_t)N);
    ASSERT_EQ(memento_proxy_outstanding_bytes(px), total);
#endif
    for (int i = 0; i < N; i++) {
        memento_proxy_free(px, ptrs[i]);
    }
    ASSERT_EQ(memento_proxy_outstanding_count(px), 0);
    memento_proxy_destroy(px);
}

int main(void) {
    printf("=== Memento Proxy Test Suite (debug=%d) ===\n\n", MEMENTO_PROXY_DEBUG);

    if (!memento_init()) {
        printf("Failed to initialize Memento\n");
        return 1;
    }

    RUN_TEST(site_id_stable);
    RUN_TEST(proxy_heap_roundtrip);
    RUN_TEST(proxy_arena_leak_report);
    RUN_TEST(proxy_pool_and_slab);
    RUN_TEST(proxy_stack_lifo);
    RUN_TEST(proxy_clean_report);
    RUN_TEST(proxy_shows_up_in_heap_report);
    RUN_TEST(proxy_null_safety);
    RUN_TEST(proxy_table_grows);

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
