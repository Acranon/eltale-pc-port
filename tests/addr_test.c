/* The address model's macros (port/addr.h, docs/MACOS.md): RDRAM is mapped where this platform keeps it
 * (0x80000000, or 0x8_80000000 on arm64 macOS) and 32-bit addresses are taken there and back.
 *     cc -std=gnu11 -O2 -Iport/include tests/addr_test.c -o addr-test && ./addr-test
 * On arm64 macOS link it where the game is linked, or RDRAM's window is the shared cache's (port/addr.h):
 *     ... -Wl,-segaddr,__TEXT,0x810000000 -Wl,-segaddr,__DATA_CONST,0x810004000 \
 *         -Wl,-segaddr,__DATA,0x810008000 -Wl,-segaddr,__LINKEDIT,0x81000c000 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#include "port/addr.h"

static int failures;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            fprintf(stderr, "addr_test: %s:%d: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                       \
        }                                                                     \
    } while (0)

/* PORT_PTR is a constant expression: static tables of RDRAM addresses use it */
static void *const table[] = { PORT_PTR(0x80000400), PORT_PTR(0x807FFFFCu) };
static void *const null_entry = PORT_PTR(0);

int main(void) {
    const uintptr_t rdram = 0x80000000u + PORT_ADDR_HI;
#ifdef __APPLE__ /* as port/p0/host.c maps it: a plain mmap hint this low is ignored */
    mach_vm_address_t at = rdram;
    unsigned char *mem = mach_vm_allocate(mach_task_self(), &at, 8 << 20, VM_FLAGS_FIXED) == KERN_SUCCESS
                             ? (unsigned char *)(uintptr_t)at : MAP_FAILED;
#else
    unsigned char *mem = mmap((void *)rdram, 8 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
    if (mem != (unsigned char *)rdram) {
        fprintf(stderr, "addr_test: cannot map RDRAM at %#llx (got %p)%s\n", (unsigned long long)rdram, (void *)mem,
                PORT_ADDR_HI ? ": link the test at 0x8_10000000 (see the top of tests/addr_test.c)" : "");
        return 1;
    }

    /* there and back: a 32-bit address, the pointer, and the pointer cut to 32 bits */
    const uint32_t a = 0x80123456u;
    unsigned char *p = PORT_PTR(a);
    CHECK(p == mem + 0x123456);
    CHECK((uint32_t)(uintptr_t)p == a);
    CHECK(PORT_ADDR_OF(p) == a);
    p[0] = 0x5A;
    CHECK(mem[0x123456] == 0x5A);

    /* the extension is the cast's: a u16 or u8 address part widens as before */
    const uint16_t lo = 0x1234;
    CHECK((uintptr_t)PORT_PTR(lo) == 0x1234u + PORT_ADDR_HI);

    /* 0 stays NULL (the game tests converted pointers against NULL), in a static table too */
    const uint32_t zero = 0;
    CHECK(PORT_PTR(zero) == NULL);
    CHECK(null_entry == NULL);
    CHECK(PORT_PTR_ANY((uintptr_t)0) == NULL);

    CHECK(table[0] == (void *)(mem + 0x400));
    CHECK(table[1] == (void *)(mem + 0x7FFFFC));

    /* Port_InRdram's range check */
    CHECK(PORT_ADDR_OF(mem) >= 0x80000000u && PORT_ADDR_OF(mem + 0x7FFFFF) < 0x80800000u);

    /* mixed words: a host pointer into RDRAM narrows to its address, a stack pointer stays whole */
    unsigned char stack_byte = 7;
    const uintptr_t stack = (uintptr_t)&stack_byte;
    CHECK(PORT_ADDR_NARROW((uintptr_t)p) == a);
    CHECK(PORT_ADDR_NARROW((uintptr_t)a) == a);
    /* a segmented label's address (&D_2000000: in the window on macOS) narrows to the segmented word */
    CHECK(PORT_ADDR_NARROW((uintptr_t)PORT_PTR(0x02000040u)) == 0x02000040u);
    CHECK(PORT_ADDR_NARROW(stack) == stack);
    CHECK(PORT_PTR_ANY((uintptr_t)a) == (void *)p);
    CHECK(PORT_PTR_ANY(stack) == (void *)&stack_byte);
    CHECK(*(unsigned char *)PORT_PTR_ANY(PORT_ADDR_NARROW(stack)) == 7);

    if (failures) return 1;
    printf("addr_test: ok (PORT_ADDR_HI %#llx)\n", (unsigned long long)PORT_ADDR_HI);
    return 0;
}
