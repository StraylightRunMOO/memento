/*
 * Leak hunting with the proxy layer.
 *
 * Wrap a heap in a proxy, route some allocations through it, "forget" a
 * few, and ask for a report. Every outstanding block comes back with the
 * exact file:line that allocated it. The proxy works in release builds
 * too — site attribution and leak accounting are always on; cookies and
 * owner-thread checks are debug-only extras (MEMENTO_PROXY_DEBUG).
 *
 * Build: cc -O2 -I../include example_leak_hunt.c -o leak_hunt -lpthread
 */

#define MEMENTO_IMPLEMENTATION
#include "memento.h"

#include <stdio.h>
#include <string.h>

typedef struct Order {
    int id;
    char customer[32];
    double total;
} Order;

static Order* order_new(memento_proxy_t* proxy, int id, const char* customer) {
    Order* o = (Order*)memento_proxy_alloc(proxy, sizeof(Order)); /* site captured */
    o->id = id;
    snprintf(o->customer, sizeof(o->customer), "%s", customer);
    o->total = 0.0;
    return o;
}

int main(void) {
    memento_init();

    memento_proxy_t* orders =
        memento_proxy_wrap_heap(memento_thread_heap_get(),
                                MEMENTO_PROXY_SITE | MEMENTO_PROXY_WATERMARK);

    Order* o1 = order_new(orders, 1, "ada");
    Order* o2 = order_new(orders, 2, "grace");
    Order* o3 = order_new(orders, 3, "edsger");

    /* We remember to free two of them... */
    memento_proxy_free(orders, o1);
    memento_proxy_free(orders, o3);

    printf("outstanding: %zu blocks, %zu bytes\n\n",
           memento_proxy_outstanding_count(orders),
           memento_proxy_outstanding_bytes(orders));

    /* ...and the report names the one we forgot, by file and line: */
    memento_proxy_report(orders, stdout);

    (void)o2; /* still leaked — that is the point of the demo */
    memento_proxy_destroy(orders);
    memento_shutdown();
    return 0;
}
