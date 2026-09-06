/*
 * shimcheck — a plain libc-malloc consumer. Under ctest it runs with
 * LD_PRELOAD=<memento_preload>, so every call below is actually memento.
 * Built and run unmodified: also passes, just boringly (on glibc).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#if defined(__linux__)
#include <malloc.h>
#endif

int main(void) {
    char* p = (char*)malloc(128);
    if (!p) return 1;
    memset(p, 'x', 128);

    p = (char*)realloc(p, 128 * 1024);
    if (!p) return 2;
    if (p[0] != 'x' || p[127] != 'x') return 3;
    memset(p, 'y', 128 * 1024);

    char* z = (char*)calloc(256, 64);
    if (!z) return 4;
    for (int i = 0; i < 256 * 64; i++) {
        if (z[i] != 0) return 5;
    }

    void* a = NULL;
    if (posix_memalign(&a, 64, 8192) != 0) return 6;
    if (((uintptr_t)a & 63) != 0) return 7;
    memset(a, 'z', 8192);

    void* aa = aligned_alloc(128, 4096);
    if (!aa) return 8;
    if (((uintptr_t)aa & 127) != 0) return 9;

    if (aligned_alloc(128, 100) != NULL) return 10; /* C11: size % align != 0 */

    char* dup = strdup("memento");
    if (!dup || strcmp(dup, "memento") != 0) return 11;
    char* ndup = strndup("memento", 3);
    if (!ndup || strcmp(ndup, "mem") != 0) return 12;

#if defined(__linux__)
    {
        size_t (*usable)(void*) = malloc_usable_size;
        if (usable(aa) < 4096) return 13;
        if (usable(NULL) != 0) return 14;
    }
#endif

    free(p);
    free(z);
    free(a);
    free(aa);
    free(dup);
    free(ndup);
    free(NULL);

    printf("shimcheck OK\n");
    return 0;
}
