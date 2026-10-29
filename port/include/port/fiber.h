#ifndef ELTALE_PORT_FIBER_H
#define ELTALE_PORT_FIBER_H

#include <stddef.h>

/* One N64 thread per fixed-stack host fiber, all on the calling OS thread. */
typedef struct PortFiber PortFiber;
PortFiber *port_fiber_main(void);
PortFiber *port_fiber_create(size_t stack_size, void (*entry)(void *), void *arg);
void port_fiber_delete(PortFiber *fiber);
void port_fiber_switch(PortFiber *from, PortFiber *to);
int port_fiber_fixed(void);

#endif
