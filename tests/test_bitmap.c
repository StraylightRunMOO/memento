/*
 * Bitmap refill vs scalar, every size class. Built with MEMENTO_IMPLEMENTATION
 * so it can call the static bmp helpers. ASan-clean when compiled with it.
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
#define ASSERT(c) do { \
    if (!(c)) { \
        printf("FAIL\n    Assertion: %s\n    File: %s:%d\n", \
               #c, __FILE__, __LINE__); \
        failed++; \
        return; \
    } \
} while(0)

static int passed, failed;

#if MEMENTO_PAGE_SIZE
TEST(bmp_first_matches_scalar) {
    uint64_t b[MEMENTO_PAGE_BMP_WORDS];
    int nwords = (int)MEMENTO_PAGE_BMP_WORDS;
    memset(b, 0, sizeof(b));
    ASSERT(memento_bmp_first_scalar(b, nwords) == -1);
    ASSERT(memento_bmp_first(b, nwords) == -1);

    b[0] = 1ull;
    ASSERT(memento_bmp_first_scalar(b, nwords) == 0);
    ASSERT(memento_bmp_first(b, nwords) == 0);

    b[0] = 0;
    b[3] = 1ull << 5;
    ASSERT(memento_bmp_first_scalar(b, nwords) == 3 * 64 + 5);
    ASSERT(memento_bmp_first(b, nwords) == 3 * 64 + 5);

    memset(b, 0, sizeof(b));
    b[nwords - 1] = 1ull << 63;
    ASSERT(memento_bmp_first(b, nwords) == memento_bmp_first_scalar(b, nwords));
}

TEST(bmp_refill_every_class) {
    memento_thread_heap_t* heap = memento_thread_heap_get();
    void* ptrs[64];
    size_t sc;
    for (sc = 0; sc < MEMENTO_SIZE_CLASS_COUNT; sc++) {
        size_t sz = memento_size_class_to_size(sc);
        int n = 64;
        int i;
        for (i = 0; i < n; i++) {
            ptrs[i] = memento_thread_heap_alloc(heap, sz);
            ASSERT(ptrs[i] != NULL);
            memset(ptrs[i], (int)sc + 1, sz < 32 ? sz : 32);
        }
        /* Free in reverse so the bitmap sees holes, not a bump run. */
        for (i = n - 1; i >= 0; i -= 2) {
            memento_thread_heap_free(heap, ptrs[i], sz);
            ptrs[i] = NULL;
        }
        for (i = 1; i < n; i += 2) {
            void* p = memento_thread_heap_alloc(heap, sz);
            ASSERT(p != NULL);
            memset(p, 0xA5, sz < 32 ? sz : 32);
            memento_thread_heap_free(heap, p, sz);
        }
        for (i = 0; i < n; i += 2) {
            if (ptrs[i]) memento_thread_heap_free(heap, ptrs[i], sz);
        }
    }
}

TEST(refill_freelist_pop_clears_bit) {
    /* Drain/flush sets the occupancy bit and links. refill's single-pop
     * used to skip bmp_clear, so the next bmp_first re-issued the live
     * block. */
    memento_thread_heap_t* heap = memento_thread_heap_get();
    size_t sz = memento_size_class_to_size(0);
    void* p;
    memento_page_t* pg;
    void* head;
    int idx;
    int nwords;
    bool fresh = false;

    memento_thread_heap_flush(heap);
    p = memento_thread_heap_alloc(heap, sz);
    ASSERT(p);
    memento_thread_heap_free(heap, p, sz);
    memento_thread_heap_flush(heap);

    pg = heap->active[0];
    ASSERT(pg != NULL);
    ASSERT(pg->free != NULL);
    head = pg->free;
    idx = memento_page_idx_of(pg, head, sz);
    ASSERT(idx >= 0 && (unsigned)idx < pg->nbits);
    ASSERT(memento_bmp_test(pg->bmp, (unsigned)idx));

    p = memento_refill_size_class(heap, 0, &fresh);
    ASSERT(p == head);
    ASSERT(!memento_bmp_test(pg->bmp, (unsigned)idx));

    nwords = (int)((pg->nbits + 63u) / 64u);
    ASSERT(memento_bmp_first(pg->bmp, nwords) != idx);

    memento_thread_heap_free(heap, p, sz);
}

TEST(no_double_handout_after_page_switch) {
    /* Fill page 0, spill onto page 1, return some page-0 blocks (bit set
     * + freelist), then keep allocating. When page 1 is exhausted, refill
     * pops page 0's freelist; a missed clear re-issues those addresses
     * on the subsequent bitmap refill. */
    memento_thread_heap_t* heap = memento_thread_heap_get();
    size_t sz = memento_size_class_to_size(0);
    enum { N = 6144 };
    void** live;
    int n = 0, i, j, freed = 0;
    memento_page_t* first_page;

    memento_thread_heap_flush(heap);
    live = (void**)malloc(sizeof(void*) * (size_t)N);
    ASSERT(live);

    live[n++] = memento_thread_heap_alloc(heap, sz);
    ASSERT(live[0]);
    first_page = memento_page_of(live[0]);

    while (n < N) {
        live[n] = memento_thread_heap_alloc(heap, sz);
        ASSERT(live[n]);
        n++;
        if (memento_page_of(live[n - 1]) != first_page) break;
    }
    for (i = 0; i < 128 && n < N; i++) {
        live[n] = memento_thread_heap_alloc(heap, sz);
        ASSERT(live[n]);
        n++;
    }

    for (i = 0; i < n && freed < 16; ) {
        if (memento_page_of(live[i]) == first_page) {
            memento_thread_heap_free(heap, live[i], sz);
            live[i] = live[--n];
            freed++;
        } else {
            i++;
        }
    }
    ASSERT(freed > 0);
    memento_thread_heap_flush(heap);

    while (n < N) {
        void* p = memento_thread_heap_alloc(heap, sz);
        ASSERT(p);
        for (j = 0; j < n; j++) {
            ASSERT(p != live[j]);
        }
        live[n++] = p;
    }

    for (i = 0; i < n; i++) memento_thread_heap_free(heap, live[i], sz);
    free(live);
}

TEST(calloc_recycled_page) {
    /* Force a recycled (non-fresh) calloc of >= 4 KiB. */
    memento_thread_heap_t* heap = memento_thread_heap_get();
    enum { N = 32 };
    void* p[N];
    int i;
    for (i = 0; i < N; i++) {
        p[i] = memento_heap_malloc(heap, 5000);
        ASSERT(p[i]);
        memset(p[i], 0x3C, 5000);
    }
    for (i = 0; i < N; i++) memento_heap_mfree(heap, p[i]);
    {
        void* z = memento_heap_calloc(heap, 1, 5000);
        ASSERT(z);
        for (i = 0; i < 5000; i++) {
            ASSERT(((unsigned char*)z)[i] == 0);
        }
        memento_heap_mfree(heap, z);
    }
}
#endif

int main(void) {
    printf("=== bitmap ===\n");
    if (!memento_init()) return 1;
#if MEMENTO_PAGE_SIZE
    RUN_TEST(bmp_first_matches_scalar);
    RUN_TEST(bmp_refill_every_class);
    RUN_TEST(refill_freelist_pop_clears_bit);
    RUN_TEST(no_double_handout_after_page_switch);
    RUN_TEST(calloc_recycled_page);
#else
    printf("  (skipped: MEMENTO_PAGE_SIZE=0)\n");
#endif
    memento_shutdown();
    printf("Passed: %d  Failed: %d\n", passed, failed);
    return failed ? 1 : 0;
}
