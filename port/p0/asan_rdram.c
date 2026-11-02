/* The AddressSanitizer build's RDRAM (docs/MEMORY_SAFETY.md). Built only by `make asan`, without
 * instrumentation and without builtins (its copy loops must not turn back into memcpy calls).
 *
 * x86-64 Linux ASan keeps the shadow of the low 2 GiB at 0x7FFF8000..0x8FFF6FFF and a protected gap
 * above it, so the N64's RDRAM at 0x80000000 lands inside ASan's own layout. Port_AsanMapRdram takes
 * those 8 MiB of shadow over (they would shadow host addresses 0x40000..0x4040000, where nothing
 * lives: the image starts at 0x10000000) and makes RDRAM's shadow, inside the gap, readable and all
 * zero: every RDRAM byte addressable. Inline checks of RDRAM accesses then pass; ASan sees nothing
 * inside RDRAM (it is one block to it), only the port's own bound below.
 *
 * ASan's memcpy/memmove/memset checks refuse any address outside its "application memory", RDRAM
 * included, so the build renames the instrumented calls (-asan-memory-access-callback-prefix) to the
 * eltale_asan_* below: a copy touching RDRAM checks its RDRAM side against RDRAM's end and its other
 * side with ASan; any other copy goes to ASan's own. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sanitizer/asan_interface.h>

void *__asan_memcpy(void *dst, const void *src, size_t size);
void *__asan_memmove(void *dst, const void *src, size_t size);
void *__asan_memset(void *dst, int c, size_t size);

static uintptr_t rdram_lo, rdram_hi;

void *Port_AsanMapRdram(unsigned long base, size_t bytes) {
    size_t scale, offset;
    __asan_get_shadow_mapping(&scale, &offset);
    uintptr_t shadow_lo = (base >> scale) + offset, shadow_hi = ((base + bytes) >> scale) + offset;
    shadow_lo &= ~(uintptr_t)0xFFF;
    shadow_hi = (shadow_hi + 0xFFF) & ~(uintptr_t)0xFFF;
    void *mem = mmap((void *)base, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (mem == MAP_FAILED) return NULL;
    void *shadow = mmap((void *)shadow_lo, shadow_hi - shadow_lo, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (shadow == MAP_FAILED) return NULL;
    rdram_lo = base;
    rdram_hi = base + bytes;
    fprintf(stderr, "[asan] RDRAM %#lx..%#lx, its shadow %#lx..%#lx (ASan shadow offset %#lx)\n", (unsigned long)base,
            (unsigned long)(base + bytes), (unsigned long)shadow_lo, (unsigned long)shadow_hi, (unsigned long)offset);
    return mem;
}

/* a report of the port's own: stderr, and the file ELTALE_ASAN_LOG names (tools/asan_run.sh) */
static void own_report(const char *line) {
    fputs(line, stderr);
    const char *path = getenv("ELTALE_ASAN_LOG");
    FILE *f = path ? fopen(path, "a") : NULL;
    if (f) {
        fputs(line, f);
        fclose(f);
    }
}

static int in_rdram(const void *p) { return (uintptr_t)p >= rdram_lo && (uintptr_t)p < rdram_hi; }

/* one side of a copy that touches RDRAM: past RDRAM's end is the port's own report, outside RDRAM ASan's */
static void check_side(const void *p, size_t size, int write, void *pc) {
    uintptr_t a = (uintptr_t)p;
    if (in_rdram(p)) {
        if (size > rdram_hi - a) {
            char line[160];
            snprintf(line, sizeof line, "[asan] RDRAM %s of %zu bytes at %#lx runs past RDRAM's end (caller pc %p)\n",
                     write ? "write" : "read", size, (unsigned long)a, pc);
            own_report(line);
            __sanitizer_print_stack_trace();
        }
        return;
    }
    void *bad = __asan_region_is_poisoned((void *)p, size);
    if (bad) __asan_report_error(pc, __builtin_frame_address(0), __builtin_frame_address(0), bad, write, size);
}

static void copy_up(unsigned char *d, const unsigned char *s, size_t n) {
    while (n >= 8 && (((uintptr_t)d | (uintptr_t)s) & 7) == 0) {
        *(uint64_t *)d = *(const uint64_t *)s;
        d += 8, s += 8, n -= 8;
    }
    while (n--) *d++ = *s++;
}

static void copy_down(unsigned char *d, const unsigned char *s, size_t n) {
    d += n, s += n;
    while (n--) *--d = *--s;
}

void *eltale_asan_memcpy(void *dst, const void *src, size_t size) {
    if (!size || (!in_rdram(dst) && !in_rdram(src))) return __asan_memcpy(dst, src, size);
    void *pc = __builtin_return_address(0);
    check_side(src, size, 0, pc);
    check_side(dst, size, 1, pc);
    copy_up(dst, src, size);
    return dst;
}

void *eltale_asan_memmove(void *dst, const void *src, size_t size) {
    if (!size || (!in_rdram(dst) && !in_rdram(src))) return __asan_memmove(dst, src, size);
    void *pc = __builtin_return_address(0);
    check_side(src, size, 0, pc);
    check_side(dst, size, 1, pc);
    if ((uintptr_t)dst <= (uintptr_t)src || (uintptr_t)dst >= (uintptr_t)src + size) copy_up(dst, src, size);
    else copy_down(dst, src, size);
    return dst;
}

void *eltale_asan_memset(void *dst, int c, size_t size) {
    if (!size || !in_rdram(dst)) return __asan_memset(dst, c, size);
    check_side(dst, size, 1, __builtin_return_address(0));
    unsigned char *d = dst;
    while (size--) *d++ = (unsigned char)c;
    return dst;
}

/* every report, and play goes on (tools/asan_run.sh sets log_path); leaks are not this build's business (the game
 * keeps everything for the whole run) */
const char *__asan_default_options(void) {
    return "detect_leaks=0:halt_on_error=0:print_suppressions=0:allocator_may_return_null=1:"
           "detect_stack_use_after_return=0:check_initialization_order=1:strict_init_order=0:"
           "detect_odr_violation=1:symbolize=1";
}

const char *__ubsan_default_options(void) { return "print_stacktrace=1:halt_on_error=0"; }
