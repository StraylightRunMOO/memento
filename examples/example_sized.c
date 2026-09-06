/*
 * The sized API: usable_size, good_size, and growing buffers without
 * copying half as often.
 *
 * memento_usable_size() tells you how much room an allocation REALLY has
 * (size classes round up); memento_good_size() tells you what to ask for
 * so nothing is wasted. Together they make the "grow a buffer" pattern
 * cheaper: grow into slack you already own before paying for a realloc.
 *
 * Build: cc -O2 -I../include example_sized.c -o sized -lpthread
 */

#define MEMENTO_IMPLEMENTATION
#include "memento.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    memento_init();

    /* good_size: the allocator's price list. Asking for 100 costs the same
     * class as asking for more — find out how much more. */
    for (size_t ask = 24; ask <= 1024; ask *= 4) {
        printf("good_size(%4zu) = %4zu\n", ask, memento_good_size(ask));
    }

    /* A dynamic buffer that exploits slack: usable_size >= requested. */
    size_t cap_request = 200;
    char* buf = (char*)memento_malloc(cap_request);
    size_t cap_real = memento_usable_size(buf);
    printf("\nasked for %zu, got %zu usable — %zu free bytes of slack\n",
           cap_request, cap_real, cap_real - cap_request);

    /* Append until we genuinely run out of room: */
    size_t len = 0;
    const char* word = "memento ";
    for (int i = 0; i < 40; i++) {
        size_t wlen = strlen(word);
        if (len + wlen + 1 > cap_real) {
            /* Truly out of room: grow to the next good size. */
            size_t new_request = memento_good_size(cap_request * 2);
            char* nb = (char*)memento_realloc(buf, new_request);
            if (!nb) { fprintf(stderr, "oom\n"); return 1; }
            buf = nb;
            cap_request = new_request;
            cap_real = memento_usable_size(buf);
            printf("grew: request %zu -> usable %zu\n", cap_request, cap_real);
        }
        memcpy(buf + len, word, wlen);
        len += wlen;
        buf[len] = 0;
    }
    printf("final: %zu chars in %zu usable bytes\n", len, cap_real);

    memento_free(buf);
    memento_shutdown();
    return 0;
}
