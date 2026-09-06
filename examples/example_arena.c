/*
 * Arena save/restore: rewindable scratch memory.
 *
 * Arenas are for phase-based work — parse a request, render a frame, run a
 * query — where everything allocated inside the phase dies together.
 * save/restore gives you a stack of checkpoints: restore() rewinds the
 * arena to the checkpoint and invalidates everything allocated since.
 *
 * Build: cc -O2 -I../include example_arena.c -o arena -lpthread
 */

#define MEMENTO_IMPLEMENTATION
#include "memento.h"

#include <stdio.h>
#include <string.h>

static char* arena_strdup(memento_arena_t* a, const char* s) {
    size_t n = strlen(s) + 1;
    char* p = (char*)memento_arena_alloc(a, n, 1);
    memcpy(p, s, n);
    return p;
}

int main(void) {
    memento_init();

    memento_arena_t* arena = memento_arena_create(64 * 1024,
                                                  memento_thread_heap_get());

    /* --- Request phase --- */
    char* query = arena_strdup(arena, "SELECT * FROM users WHERE");
    printf("query:   %s\n", query);
    printf("used:    %zu bytes\n", memento_arena_used(arena));

    memento_arena_save_t checkpoint = memento_arena_save(arena);

    /* --- Try an optimization: speculative work we might throw away --- */
    char* speculation = arena_strdup(arena, " AND cached_index_is_valid = 1");
    printf("spec:    %s%s\n", query, speculation);
    printf("used:    %zu bytes\n", memento_arena_used(arena));

    /* Speculation failed? Rewind. Every byte allocated since the save
     * point is gone — no per-object frees, no leak tracking. */
    memento_arena_restore(arena, &checkpoint);
    printf("after restore: %zu bytes used\n", memento_arena_used(arena));
    printf("query still valid: %s\n", query); /* allocated before the save */

    /* --- Request over: everything dies at once --- */
    memento_arena_reset(arena);
    printf("after reset: %zu bytes used (capacity retained: %zu)\n",
           memento_arena_used(arena), memento_arena_capacity(arena));

    memento_arena_destroy(arena);
    memento_shutdown();
    return 0;
}
