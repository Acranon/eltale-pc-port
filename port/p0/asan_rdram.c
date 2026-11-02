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
 * side with ASan; any other copy goes to ASan's own.
 *
 * ELTALE_ASAN_RDRAM=loaded (a deeper look into RDRAM): RDRAM starts poisoned but for the IPL copy of the
 * main segment (0x80000000..0x80100400) and the port's own pages (0x807F0000..: fonts, the title strip), and
 * each ROM DMA unpoisons what it fills (osPiStartDma calls Port_AsanRdramLoaded). Then a read or write of
 * RDRAM that nothing loaded, as past the end of a loaded block, is an ASan report (use-after-poison), mem*
 * copies included. RDRAM the game fills itself (not by DMA) shows up too: each report needs a look. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sanitizer/asan_interface.h>

void *__asan_memcpy(void *dst, const void *src, size_t size);
void *__asan_memmove(void *dst, const void *src, size_t size);
void *__asan_memset(void *dst, int c, size_t size);

static uintptr_t rdram_lo, rdram_hi;
static int poison_mode;
static unsigned char *rdram_shadow; /* RDRAM's shadow bytes, one per 8 RDRAM bytes */

/* (not memset: ASan's memset checks its range, and shadow is not application memory) */
static void fill(unsigned char *p, unsigned char v, size_t n) {
    while (n--) *p++ = v;
}

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
    rdram_shadow = (unsigned char *)((base >> scale) + offset);
    const char *mode = getenv("ELTALE_ASAN_RDRAM");
    poison_mode = mode && !strcmp(mode, "loaded");
    if (poison_mode) {
        /* (ASan's own poisoning refuses addresses outside its application memory: the shadow bytes by hand, 0xF7 its
         * user-poisoned mark) */
        fill(rdram_shadow + (0x100400 >> 3), 0xF7, (0x7F0000 - 0x100400) >> 3);
        /* the game's own RDRAM buffers, written but never loaded: the two frame buffers (Gfx_InitFrames clears them) */
        fill(rdram_shadow + (0x3B5000 >> 3), 0, (320 * 240 * 2) >> 3);
        fill(rdram_shadow + (0x3DA800 >> 3), 0, (320 * 240 * 2) >> 3);
        fprintf(stderr, "[asan] RDRAM poisoned until loaded (ELTALE_ASAN_RDRAM=loaded)\n");
    }
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

/* a ROM DMA filled [dst, dst + size) (host.c osPiStartDma) */
void Port_AsanRdramLoaded(void *dst, size_t size) {
    /* whole 8-byte granules: a block's last few bytes may come with up to 7 after it */
    if (poison_mode && in_rdram(dst) && size && size <= rdram_hi - (uintptr_t)dst)
        fill(rdram_shadow + (((uintptr_t)dst - rdram_lo) >> 3), 0, (((uintptr_t)dst - rdram_lo + size - 1) >> 3) -
                                                                      (((uintptr_t)dst - rdram_lo) >> 3) + 1);
}

/* ELTALE_ASAN_RDRAM=loaded: the first poisoned byte of an RDRAM range (0 when none) */
static uintptr_t rdram_poisoned(uintptr_t a, size_t size) {
    if (!poison_mode) return 0;
    for (uintptr_t p = a; p < a + size; p++) {
        signed char v = (signed char)rdram_shadow[(p - rdram_lo) >> 3];
        if (v != 0 && (v < 0 || (signed char)(p & 7) >= v)) return p;
    }
    return 0;
}

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
            return;
        }
        uintptr_t bad = rdram_poisoned(a, size);
        if (bad) __asan_report_error(pc, __builtin_frame_address(0), __builtin_frame_address(0), (void *)bad, write, size);
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
