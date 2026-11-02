#ifndef PORT_SANITIZE_H
#define PORT_SANITIZE_H

/* The AddressSanitizer build (docs/MEMORY_SAFETY.md, `make asan`): PORT_ASAN is 1 when the compiler
 * instruments this file for ASan. Nothing here changes a normal build. */
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PORT_ASAN 1
#endif
#endif
#if !defined(PORT_ASAN) && defined(__SANITIZE_ADDRESS__)
#define PORT_ASAN 1
#endif
#ifndef PORT_ASAN
#define PORT_ASAN 0
#endif

#if PORT_ASAN
#include <sanitizer/asan_interface.h>
#include <sanitizer/common_interface_defs.h>
#define port_unpoison(p, n) __asan_unpoison_memory_region((p), (n))
#include <stddef.h>
/* port/p0/asan_rdram.c: maps the 8 MiB of RDRAM at `base` inside ASan's address layout (x86-64 Linux keeps
 * its low shadow where RDRAM goes) with its shadow all addressable; NULL when it cannot */
void *Port_AsanMapRdram(unsigned long base, size_t bytes);
#endif

#endif
