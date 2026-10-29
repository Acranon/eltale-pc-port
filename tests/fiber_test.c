#include "port/fiber.h"

#include <stdio.h>
#include <stdlib.h>

static PortFiber *host;
static PortFiber *a;
static PortFiber *b;
static int sequence[4];
static int next;

__attribute__((noreturn)) void Port_HostFatal(const char *message) {
    fprintf(stderr, "fiber test: %s\n", message);
    exit(1);
}

static void fiber_a(void *arg) {
    (void)arg;
    sequence[next++] = 1;
    port_fiber_switch(a, host);
    sequence[next++] = 3;
    port_fiber_switch(a, host);
    Port_HostFatal("A resumed after completion");
}

static void fiber_b(void *arg) {
    (void)arg;
    sequence[next++] = 2;
    port_fiber_switch(b, host);
    sequence[next++] = 4;
    port_fiber_switch(b, host);
    Port_HostFatal("B resumed after completion");
}

int main(void) {
    host = port_fiber_main();
    a = port_fiber_create(0x10000, fiber_a, NULL);
    b = port_fiber_create(0x10000, fiber_b, NULL);
    port_fiber_switch(host, a);
    port_fiber_switch(host, b);
    port_fiber_switch(host, a);
    port_fiber_switch(host, b);
    if (next != 4 || sequence[0] != 1 || sequence[1] != 2 ||
        sequence[2] != 3 || sequence[3] != 4)
        Port_HostFatal("wrong switch order");
    port_fiber_delete(a);
    port_fiber_delete(b);
    puts("FIBER_OK");
    return 0;
}
