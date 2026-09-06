/*
 * Worker pool with park/adopt: thread heaps as a renewable resource.
 *
 * Every thread gets its own heap (no locks, no atomics on the hot path).
 * When a worker is done it PARKS its heap instead of destroying it; the
 * next worker ADOPTS a parked heap — warm caches, warm spans, no fresh
 * mmap storm. On a NUMA box, adopt prefers a heap from the caller's own
 * node when one is parked.
 *
 * Build: cc -O2 -I../include example_worker_pool.c -o worker_pool -lpthread
 */

#define MEMENTO_IMPLEMENTATION
#include "memento.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define WORK_ITEMS 20000

static void* worker(void* arg) {
    int id = *(int*)arg;

    /* Take a parked heap if one is waiting, else create a fresh one. */
    memento_thread_heap_t* heap = memento_heap_adopt();
    int reused = (heap != NULL);
    if (!heap) heap = memento_thread_heap_get();

    /* Do some bursty work: allocate a batch, chew on it, free it. */
    for (int round = 0; round < 4; round++) {
        /* Exact-mode frees need the true size, so each batch picks one
         * size and sticks to it — the classic batch pattern. */
        size_t n = 32 + (size_t)(rand() % 224);
        char* batch[WORK_ITEMS / 4];
        for (int i = 0; i < WORK_ITEMS / 4; i++) {
            batch[i] = (char*)memento_thread_heap_alloc(heap, n);
            memset(batch[i], id, n);
        }
        for (int i = 0; i < WORK_ITEMS / 4; i++) {
            memento_thread_heap_free(heap, batch[i], n);
        }
    }

    memento_heap_stats_t st;
    memento_thread_heap_stats(heap, &st);
    printf("worker %d: %s heap, %zu allocs, peak %zu KiB, spans %zu\n",
           id, reused ? "adopted" : "fresh",
           st.alloc_count, st.bytes_peak / 1024, st.span_count);

    /* Park, don't destroy: the heap keeps its spans warm for the next worker. */
    memento_heap_park();
    return NULL;
}

int main(void) {
    memento_init();

    /* Two waves of workers — the second wave inherits the first's heaps. */
    pthread_t threads[3];
    int ids[3] = {0, 1, 2};
    for (int wave = 0; wave < 2; wave++) {
        printf("-- wave %d --\n", wave + 1);
        for (int i = 0; i < 3; i++) {
            pthread_create(&threads[i], NULL, worker, &ids[i]);
        }
        for (int i = 0; i < 3; i++) {
            pthread_join(threads[i], NULL);
        }
    }

    memento_shutdown();
    return 0;
}
