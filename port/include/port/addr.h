/* The address model (docs/MACOS.md).
 *
 * Display-list words, the game's records and its raw D_ labels hold 32-bit addresses: RDRAM at
 * 0x80000000..0x80800000 (N64 bytes) and the game's own image from 0x10000000 (native data). On Windows
 * and Linux both sit there, below 4 GiB, so a 32-bit address is the pointer. arm64 macOS keeps the whole
 * low 4 GiB unmapped, so there both sit in a 4 GiB window higher up, at PORT_ADDR_HI: RDRAM at
 * 0x8_80000000, the image at 0x8_10000000. Their low 32 bits are the same, so a pointer cut to 32 bits is
 * still its address (display lists, osVirtualToPhysical), but a 32-bit address turned back into a pointer
 * needs PORT_ADDR_HI put back.
 *
 * Why 0x8_00000000: the window must be free of everything else. 0x1_00000000 holds malloc's small blocks
 * and the stack, and the dyld shared cache reserves 0x1_80000000..0xF_C0000000 for a program linked in the
 * usual place. A program whose image is linked outside 0x1_ (ours, at 0x8_10000000) gets the shared cache
 * at 0x260_00000000 instead, which leaves 0x2_..0xF_ empty; mmap hands out memory from 0x70_00000000 up. The
 * heap staying in 0x1_ also means a heap pointer can never pass for an image or RDRAM address
 * (osVirtualToPhysical refuses it). RDRAM is taken with a fixed Mach allocation that fails rather than
 * replace anything. tools/gen_abs_symbols.py reads the value from this file.
 *
 * PORT_ADDR_HI is 0 everywhere else, and both macros then compile to the plain casts they replace (the
 * same sign or zero extension, nothing added), so Windows and Linux build the same code as before. A
 * 32-bit address made into a pointer without PORT_PTR on macOS lands in the unmapped low 4 GiB, and the
 * first access to it faults at once.
 */
#ifndef ELTALE_PORT_ADDR_H
#define ELTALE_PORT_ADDR_H

#include <stdint.h>

#if defined(__APPLE__) && defined(__aarch64__)
#define PORT_ADDR_HI 0x800000000ull
#else
#define PORT_ADDR_HI 0ull
#endif

/* A 32-bit RDRAM or image address (an integer) as a host pointer; a constant expression when `a` is a
 * constant, so static tables can use it. Address 0 stays NULL, as the plain cast gives it, so the game's
 * `if (p == NULL)` after a conversion holds everywhere. (`* 1` makes passing a pointer a compile error.
 * On arm64 macOS `a` is read twice: give it no side effects; elsewhere the test folds away unread.) */
#define PORT_PTR(a) \
    ((void *)((uintptr_t)((a) * 1) + (PORT_ADDR_HI && (a) ? (uintptr_t)PORT_ADDR_HI : (uintptr_t)0)))

/* A host pointer into RDRAM or the image as its 32-bit address, kept as a uintptr_t: for range checks
 * and offsets against 0x80000000-style constants. */
#define PORT_ADDR_OF(p) ((uintptr_t)(p) - (uintptr_t)PORT_ADDR_HI)

/* For words that may hold either a 32-bit address or a whole host pointer (a stack or heap buffer: the
 * native 2D draw's entry points take both). PORT_ADDR_NARROW makes a host pointer into the window (the
 * image, RDRAM, or a raw label: a segmented one such as &D_2000000 too) its 32-bit address and leaves
 * anything else as it is;
 * PORT_PTR_ANY makes a 32-bit address a pointer (0 stays NULL) and leaves a whole host pointer as it is.
 * Both read `w` twice. */
#define PORT_ADDR_NARROW(w) \
    ((uintptr_t)(w) - (PORT_ADDR_HI && (uintptr_t)(w) - (uintptr_t)PORT_ADDR_HI <= 0xFFFFFFFFu ? (uintptr_t)PORT_ADDR_HI : 0))
#define PORT_PTR_ANY(w) \
    ((void *)((uintptr_t)(w) +                                                                         \
              (PORT_ADDR_HI && (uintptr_t)(w) && (uintptr_t)(w) <= 0xFFFFFFFFu ? (uintptr_t)PORT_ADDR_HI : 0)))

#endif
