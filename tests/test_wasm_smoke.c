/*
 * Wasm/Emscripten smoke: init, sized alloc, guarded arena, free, shutdown.
 * Compiles natively too.
 *
 *   emcc -O2 -Iinclude tests/test_wasm_smoke.c -o /tmp/memento_wasm.js
 *   node /tmp/memento_wasm.js
 */
#define MEMENTO_IMPLEMENTATION
#include "memento.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    if (!memento_init()) {
        fprintf(stderr, "memento_init failed\n");
        return 1;
    }
    if (strcmp(memento_version_string(), "3.1.1") != 0) {
        fprintf(stderr, "version %s\n", memento_version_string());
        return 1;
    }

    void *p = memento_malloc(256);
    if (!p) {
        fprintf(stderr, "memento_malloc failed\n");
        return 1;
    }
    memset(p, 0x5a, 256);

    memento_thread_heap_t *heap = memento_thread_heap_get();
    memento_arena_t *arena = memento_arena_create_guarded(64 * 1024, heap);
    if (!arena) {
        fprintf(stderr, "guarded arena failed\n");
        return 1;
    }
    void *q = memento_arena_alloc(arena, 128, 8);
    if (!q) {
        fprintf(stderr, "arena_alloc failed\n");
        return 1;
    }
    memset(q, 0xa5, 128);
    memento_arena_destroy(arena);

    memento_free(p);
    memento_shutdown();
    printf("memento wasm smoke ok (%s)\n", memento_version_string());
    return 0;
}
