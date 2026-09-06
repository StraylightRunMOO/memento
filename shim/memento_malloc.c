/*
 * memento_malloc.c — malloc interposition shim
 *
 * Build as a shared library and LD_PRELOAD it into anything whose allocator
 * you don't trust (musl containers, third-party C that "doesn't know the
 * size", The Mess):
 *
 *   gcc -O2 -shared -fPIC -I../include memento_malloc.c -o memento_malloc.so -lpthread
 *   LD_PRELOAD=./memento_malloc.so ./your_binary
 *
 * Or link it straight into your binary — defining malloc in the executable
 * interposes libc's just the same.
 *
 * Behavior:
 * - malloc/free/calloc/realloc/reallocarray/aligned_alloc/posix_memalign/
 *   memalign/valloc/malloc_usable_size route through the sized API.
 * - kill -USR1 <pid> dumps a per-heap report to stderr.
 * - MEMENTO_DUMP_ATEXIT=1 dumps the same report when the process exits.
 *
 * Internal bookkeeping never touches libc malloc (that would recurse): the
 * few control blocks memento needs are served by a tiny mmap bump path below.
 *
 * License: MIT
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* MAP_ANONYMOUS, sigaction, reallocarray under strict C11 */
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#error "memento_malloc.c is a POSIX LD_PRELOAD shim; on Windows use the API directly."
#endif

#include <sys/mman.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

/* --------------------------------------------------------------------------
 * Bootstrap allocator for memento's own control blocks (heap structs, proxy
 * state). mmap-only, with the mapping size stashed next to the user pointer
 * so free needs no size.
 * ------------------------------------------------------------------------ */

static void* memento_boot_alloc(size_t size) {
    size_t total = (size + 16 + 4095) & ~(size_t)4095;
    char* base = (char*)mmap(NULL, total, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return NULL;
    *(size_t*)base = total;
    return base + 16;
}

static void memento_boot_free(void* ptr, size_t size) {
    (void)size;
    if (!ptr) return;
    char* base = (char*)ptr - 16;
    munmap(base, *(size_t*)base);
}

static void* memento_boot_aligned(size_t align, size_t size) {
    (void)align; /* a page-aligned mapping satisfies any align <= 4096, and
                    memento only ever asks for its cache line (64) here */
    size_t total = (size + 4096 + 4095) & ~(size_t)4095;
    char* base = (char*)mmap(NULL, total, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return NULL;
    char* user = base + 4096;
    *(size_t*)base = total;
    *(void**)(user - 8) = base;
    return user;
}

static void memento_boot_free_aligned(void* ptr) {
    if (!ptr) return;
    char* base = *(char**)((char*)ptr - 8);
    munmap(base, *(size_t*)base);
}

#define MEMENTO_MALLOC(size)            memento_boot_alloc(size)
#define MEMENTO_FREE(ptr, size)         memento_boot_free((ptr), (size))
#define MEMENTO_ALIGNED_ALLOC(a, s)     memento_boot_aligned((a), (s))
#define MEMENTO_ALIGNED_FREE(ptr)       memento_boot_free_aligned(ptr)

#define MEMENTO_IMPLEMENTATION
#include "memento.h"

/* --------------------------------------------------------------------------
 * Lazy init + reporting
 * ------------------------------------------------------------------------ */

static pthread_once_t memento_shim_once = PTHREAD_ONCE_INIT;

static void memento_shim_dump(void) {
    memento_report_all(stderr);
}

static void memento_shim_on_sigusr1(int sig) {
    (void)sig;
    /* Not async-signal-safe (takes the registry lock, calls stdio). That is
     * the accepted deal for a debug dump: if a heap op was in flight on this
     * thread when the signal landed, attach gdb instead. */
    memento_shim_dump();
}

static void memento_shim_bootstrap(void) {
    memento_init();
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = memento_shim_on_sigusr1;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    const char* dump = getenv("MEMENTO_DUMP_ATEXIT");
    if (dump && dump[0] == '1') {
        atexit(memento_shim_dump);
    }
}

static void memento_shim_ensure(void) {
    pthread_once(&memento_shim_once, memento_shim_bootstrap);
}

/* --------------------------------------------------------------------------
 * Interposed entry points
 * ------------------------------------------------------------------------ */

void* malloc(size_t size) {
    memento_shim_ensure();
    return memento_malloc(size);
}

void free(void* ptr) {
    if (!ptr) return; /* free(NULL) must not force init */
    memento_shim_ensure();
    memento_free(ptr);
}

void* calloc(size_t count, size_t size) {
    memento_shim_ensure();
    return memento_calloc(count, size);
}

void* realloc(void* ptr, size_t new_size) {
    memento_shim_ensure();
    return memento_realloc(ptr, new_size);
}

void* reallocarray(void* ptr, size_t count, size_t size) {
    memento_shim_ensure();
    size_t total;
    if (count && size > (size_t)-1 / count) return NULL;
    total = count * size;
    return memento_realloc(ptr, total);
}

void* aligned_alloc(size_t alignment, size_t size) {
    memento_shim_ensure();
    return memento_aligned_alloc(alignment, size);
}

int posix_memalign(void** out, size_t alignment, size_t size) {
    memento_shim_ensure();
    return memento_posix_memalign(out, alignment, size);
}

void* memalign(size_t alignment, size_t size) {
    memento_shim_ensure();
    return memento_aligned_alloc(alignment, size);
}

void* valloc(size_t size) {
    memento_shim_ensure();
    return memento_aligned_alloc(4096, size);
}

void* pvalloc(size_t size) {
    memento_shim_ensure();
    return memento_aligned_alloc(4096, (size + 4095) & ~(size_t)4095);
}

size_t malloc_usable_size(void* ptr) {
    return memento_usable_size(ptr);
}

void cfree(void* ptr) {
    free(ptr);
}
