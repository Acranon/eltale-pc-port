/* The reference for port/audio/aspmain.c (tests only, never the play path): the RSP interpreter
 * (port/rsp) running the game's own aspMain microcode (text and data from the user's ROM, in RDRAM)
 * on the game's audio tasks, compared with the native code that plays them.
 *
 * ELTALE_AUDIO_CHECK=cmd (or 1)  every command run by both from the same DMEM and DRAM; each one that
 *                                differs is reported (first differing halfword and the count).
 * ELTALE_AUDIO_CHECK=task        each whole task run by both, independently, each from its own
 *                                state (the microcode's DMEM persists on its own from task to task):
 *                                every DRAM byte either side wrote and the buffers in DMEM. A task
 *                                that differs is run again a command at a time to name the command.
 *                                Also times the microcode (P1_AUDIO_TIME; ELTALE_AUDIO_TIME=1 times
 *                                the native code in play).
 * ELTALE_AUDIO_FUZZ=k            (with either) every k-th task is first run as a mutated copy, a
 *                                command at a time: random volumes, rates, targets, pitches, gains,
 *                                filter and ADPCM states, INIT/LOOP flags and full-scale noise in the
 *                                buffers; everything it writes is undone. ELTALE_AUDIO_FUZZ_SEED.
 * In every mode the microcode's writes are undone, so the game always hears the native code: the
 * game's hashes are the same as without the check. The summary (P1_AUDIO_CHECK, P1_AUDIO_COVER) goes
 * to stderr at the end.
 *
 * The microcode sees memory big-endian; the native game keeps it in host order. The DMA converts by
 * what the memory holds: sample data the PI copied from the ROM is bytes, the task's command list
 * is 32-bit words, and everything else the audio code hands the RSP (states, books, mix and output
 * buffers) is 16-bit samples. A DRAM address is 24 bits on the RSP, and the audio heap and arenas sit
 * anywhere in the image (0x10000000 on, more than 16 MiB), so the reference gives the microcode
 * windows: each 1 MiB of the image a command names is seen at a 2 MiB window of the RSP's 16 MiB. */
#include "ultra64.h"
#include "port/audio.h"
#include "../rsp/rsp.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((noreturn)) void Port_HostFatal(const char *what);
uint64_t Port_HostNowNs(void);
extern void (*Port_AspWriteHook)(void *dst, u32 bytes);

/* ---- what host memory holds: ROM bytes (one bit per 8 bytes of the image's first 16 MiB) ---- */

#define IMAGE_BASE 0x10000000u
#define IMAGE_SPAN (64u << 20)
static uint8_t rom_bytes_map[IMAGE_SPAN / 8 / 8];

void Port_LleNotePiDma(void *dst, u32 size) {
    uint32_t a = (uint32_t)(uintptr_t)dst;
    if (a < IMAGE_BASE || a - IMAGE_BASE + size > IMAGE_SPAN) return;
    for (uint32_t b = (a - IMAGE_BASE) >> 3, e = (a - IMAGE_BASE + size + 7) >> 3; b < e; b++)
        rom_bytes_map[b >> 3] |= 1 << (b & 7);
}

/* the command list being run, with its DRAM addresses made 24-bit: aspMain takes the whole top
 * byte as the segment number, so 0x10xxxxxx would index past its segment table */
static uint32_t list_start, list_end;
static uint32_t list_copy[0x8000 / 4];

static int in_image(uint32_t a) { return a >= IMAGE_BASE && a - IMAGE_BASE < IMAGE_SPAN; }

/* the RSP's 16 MiB as 8 windows of 2 MiB, each showing the image from a 1 MiB boundary (so a DMA of up
 * to 1 MiB never runs off one), given out as commands name addresses and kept */
#define WINDOWS 8
static uint32_t window_base[WINDOWS];
static unsigned window_count;

static uint32_t to_rsp(uint32_t host) {
    uint32_t base = host & ~0xFFFFFu;
    unsigned w;
    for (w = 0; w < window_count; w++)
        if (host - window_base[w] < (2u << 20)) return w << 21 | (host - window_base[w]);
    if (window_count == WINDOWS) Port_HostFatal("RSP reference: the audio lists name too much of the image");
    window_base[window_count] = base;
    return window_count++ << 21 | (host - base);
}
static uint32_t to_host(uint32_t rsp_addr) {
    rsp_addr &= 0xFFFFFF;
    if ((rsp_addr >> 21) >= window_count) Port_HostFatal("RSP reference: DMA outside its windows");
    return window_base[rsp_addr >> 21] + (rsp_addr & 0x1FFFFF);
}

/* host byte address of big-endian byte `a` (a host address) */
static uint8_t *host_byte(uint32_t a) {
    if (!in_image(a)) Port_HostFatal("RSP DMA outside the image");
    uint32_t o = a - IMAGE_BASE;
    if (rom_bytes_map[o >> 6] & (1 << ((o >> 3) & 7))) return (uint8_t *)(uintptr_t)a;
    if (a >= list_start && a < list_end) return (uint8_t *)list_copy + ((a - list_start) ^ 3);
    return (uint8_t *)(uintptr_t)(a ^ 1);
}

/* ---- write logs: the microcode's (to undo and compare) and the native code's ---- */

typedef struct { uint32_t addr; uint8_t old, lle; } LleWrite;
static LleWrite *wlog;
static size_t wlog_count, wlog_cap;
static int logging;

typedef struct { uint8_t *p; uint8_t old; } HostWrite; /* native writes and the fuzzer's, to undo */
static HostWrite *hlog;
static size_t hlog_count, hlog_cap;

static void *grow(void *p, size_t *cap, size_t size) {
    *cap = *cap ? *cap * 2 : 1 << 16;
    p = realloc(p, *cap * size);
    if (!p) Port_HostFatal("out of memory");
    return p;
}

static void host_write(uint32_t a, uint8_t v) {
    uint8_t *p = host_byte(a);
    if (logging) {
        if (wlog_count == wlog_cap) wlog = grow(wlog, &wlog_cap, sizeof *wlog);
        wlog[wlog_count++] = (LleWrite){ a, *p, v };
    }
    *p = v;
}

static void undo_lle(void) {
    for (size_t k = wlog_count; k-- > 0;) *host_byte(wlog[k].addr) = wlog[k].old;
}

static void note_host_write(void *dst, u32 bytes) {
    for (u32 i = 0; i < bytes; i++) {
        if (hlog_count == hlog_cap) hlog = grow(hlog, &hlog_cap, sizeof *hlog);
        hlog[hlog_count++] = (HostWrite){ (uint8_t *)dst + i, ((uint8_t *)dst)[i] };
    }
}

static void undo_host(size_t from) {
    while (hlog_count > from) { hlog_count--; *hlog[hlog_count].p = hlog[hlog_count].old; }
}

/* ---- the RSP (port/rsp), with DRAM at the image's 24-bit addresses ---- */

static Rsp rsp;
static uint8_t *const dmem = rsp.dmem;
static uint8_t *const imem = rsp.imem;

static uint8_t dram_read(void *ctx, uint32_t a) { (void)ctx; return *host_byte(to_host(a)); }
static void dram_write(void *ctx, uint32_t a, uint8_t v) { (void)ctx; host_write(to_host(a), v); }

static void store(uint32_t a, int bytes, uint32_t v) {
    for (int i = bytes - 1; i >= 0; i--, v >>= 8) dmem[(a + i) & 0xFFF] = (uint8_t)v;
}

/* what rspboot leaves: the microcode text at IMEM 0x080, its data at DMEM 0 */
static void load_ucode(const OSTask *task) {
    memcpy(imem + 0x80, (const void *)(uintptr_t)((uint32_t)(uintptr_t)task->t.ucode | 0x80000000u), 0x1000 - 0x80);
    memcpy(dmem, (const void *)(uintptr_t)((uint32_t)(uintptr_t)task->t.ucode_data | 0x80000000u), task->t.ucode_data_size);
}

/* Run the `words` command words `cmds` as a list at host address `at` (the task's list, or one
 * command of it; `cmds` may be a mutated copy). */
static void run_list(const OSTask *task, const uint32_t *at, const uint32_t *cmds, uint32_t words) {
    uint32_t tw[16] = { 0 };
    tw[0] = task->t.type;
    tw[1] = task->t.flags;
    tw[12] = to_rsp((uint32_t)(uintptr_t)at); /* data_ptr, as the RSP sees it */
    tw[13] = words * 4;
    for (int i = 0; i < 16; i++) store(0xFC0 + i * 4, 4, tw[i]);
    list_start = (uint32_t)(uintptr_t)at;
    list_end = list_start + words * 4;
    if (words * 4 > sizeof list_copy) Port_HostFatal("RSP: audio command list too long");
    for (uint32_t i = 0; i < words; i += 2) {
        uint32_t w0 = cmds[i], w1 = cmds[i + 1], op = w0 >> 24;
        /* ADPCM ENVMIXER LOADBUFF RESAMPLE SAVEBUFF LOADADPCM POLEF SETLOOP carry an address */
        if (((1u << op) & 0xC87A) && in_image(w1)) w1 = to_rsp(w1);
        list_copy[i] = w0;
        list_copy[i + 1] = w1;
    }
    rsp.ctx = NULL;
    rsp.dram_read = dram_read;
    rsp.dram_write = dram_write;
    rsp.fatal = Port_HostFatal;
    Rsp_Reset(&rsp, 0x80);
    rsp.r[1] = 0xFC0; /* rspboot leaves the task's DMEM address in $1 */
    if (!Rsp_Run(&rsp, 50000000)) Port_HostFatal("RSP: aspMain did not finish");
}

/* ---- comparing ----
 * The microcode's buffers start at DMEM 0x5C0 (its SETBUFF adds that); the native code keeps them at
 * the command lists' own offsets, in host order. A halfword matches if it reads the same as a sample,
 * or holds the same two bytes (ADPCM data straight from the ROM). */
#define BUF_BASE 0x5C0
#define BUF_BYTES (0xFC0 - BUF_BASE)

static const char *const names[16] = { "SPNOOP", "ADPCM", "CLEARBUFF", "ENVMIXER", "LOADBUFF", "RESAMPLE",
                                       "SAVEBUFF", "SEGMENT", "SETBUFF", "SETVOL", "DMEMMOVE", "LOADADPCM",
                                       "MIXER", "INTERLEAVE", "POLEF", "SETLOOP" };

static int mode; /* 1 a command at a time, 2 whole tasks */
static unsigned fuzz_every, fuzz_tasks, fuzz_cmds, fuzz_bad[16];
static uint64_t fuzz_rng = 0x9E3779B97F4A7C15ull;
static unsigned check_tasks, check_cmds, check_bad[16], bad_tasks, check_printed;
static unsigned cover[16][256], cover_odd[16];
static uint64_t time_lle_ns;
static const char *phase = "task";

static int dmem_same(const uint8_t *h, uint32_t off) {
    const uint8_t *l = dmem + BUF_BASE + off;
    return (h[off] == l[1] && h[off + 1] == l[0]) || (h[off] == l[0] && h[off + 1] == l[1]);
}

static void sync_dmem(void) {
    const uint8_t *h = Port_AspDmem();
    for (uint32_t off = 0; off < BUF_BYTES; off += 2)
        if (!dmem_same(h, off)) { dmem[BUF_BASE + off] = h[off + 1]; dmem[BUF_BASE + off + 1] = h[off]; }
}

/* The fuzzer's sync, before each command: every buffer halfword given to the microcode as the sample
 * the native side holds, except an ADPCM command's input, which is bytes. (Buffers hold samples, which
 * the two sides keep in opposite byte orders, or ADPCM bytes, kept alike; a mutated list may read one
 * as the other, which the game never does. This gives both sides the same inputs whatever was there.) */
static void sync_dmem_for(uint32_t w0) {
    const uint8_t *h = Port_AspDmem();
    for (uint32_t off = 0; off < BUF_BYTES; off += 2) {
        dmem[BUF_BASE + off] = h[off + 1];
        dmem[BUF_BASE + off + 1] = h[off];
    }
    if (w0 >> 24 != 1) return;
    uint32_t in = (dmem[0x360] << 8 | dmem[0x361]) - BUF_BASE, count = dmem[0x364] << 8 | dmem[0x365];
    /* the frames, the next one the microcode reads ahead, in whole halfwords */
    uint32_t from = in & 0xFFFE, to = in + (count + 31) / 32 * 9 + 10;
    /* noise headers name predictors 0-7 only: 8-15 are books in the buffers' first bytes, which
     * may be these very bytes, read as samples */
    for (uint32_t f = in; f < to; f += 9) ((uint8_t *)h)[(f & 0xFFFF) % BUF_BYTES] &= ~0x08;
    for (uint32_t i = from; i < to + (to & 1); i++) dmem[BUF_BASE + (i & 0xFFFF) % BUF_BYTES] = h[(i & 0xFFFF) % BUF_BYTES];
}

static unsigned printed_by_op[16];
static void report(const char *what, uint32_t cmd, uint32_t w0, uint32_t w1, const char *where, uint32_t at,
                   int hv, int lv, unsigned n) {
    if (check_printed++ >= 60 || printed_by_op[w0 >> 24]++ >= 8) return; /* the first few of each */
    fprintf(stderr, "P1_AUDIO_CHECK %s=%u cmd=%u %s %08x %08x: %s differ at %08x (native %d / microcode %d, %u)\n",
            phase, check_tasks, cmd, what, w0, w1, where, at, hv, lv, n);
    if (where[1] == 'M') { /* DMEM: the 16 samples from the first differing one's 32-byte block */
        const uint8_t *h = Port_AspDmem();
        uint32_t a = at & ~0x1Fu;
        fprintf(stderr, "  native    %04x:", a);
        for (uint32_t i = 0; i < 32; i += 2) fprintf(stderr, " %d", *(const int16_t *)(h + ((a + i) & 0xFFE)));
        fprintf(stderr, "\n  microcode %04x:", a);
        for (uint32_t i = 0; i < 32; i += 2) {
            uint32_t d = BUF_BASE + ((a + i) & 0xFFE);
            fprintf(stderr, " %d", (int16_t)(dmem[d & 0xFFF] << 8 | dmem[(d + 1) & 0xFFF]));
        }
        fprintf(stderr, "\n");
    }
}

/* DMEM after a command or task: the first differing halfword and how many */
static int compare_dmem(uint32_t cmd, uint32_t w0, uint32_t w1) {
    const uint8_t *h = Port_AspDmem();
    unsigned n = 0;
    uint32_t first = 0;
    for (uint32_t off = 0; off < BUF_BYTES; off += 2)
        if (!dmem_same(h, off) && !n++) first = off;
    if (n)
        report(names[w0 >> 24], cmd, w0, w1, "DMEM halfwords", first, *(const int16_t *)(h + first),
               (int16_t)(dmem[BUF_BASE + first] << 8 | dmem[BUF_BASE + first + 1]), n);
    return n != 0;
}

/* DRAM after a command or task, with the microcode's writes undone and the native code's done: the
 * last value the microcode wrote to each byte against what is there now, and every byte only the
 * native code changed (from hlog[from..]). */
static uint8_t seen[IMAGE_SPAN / 8];
static int compare_dram(uint32_t cmd, uint32_t w0, uint32_t w1, size_t from) {
    size_t n = 0;
    uint32_t first = 0;
    int hv = 0, lv = 0;
    for (size_t k = wlog_count; k-- > 0;) {
        uint8_t *p = host_byte(wlog[k].addr);
        uint32_t o = (uint32_t)(uintptr_t)p - IMAGE_BASE;
        if (seen[o >> 3] & (1 << (o & 7))) continue;
        seen[o >> 3] |= 1 << (o & 7);
        if (*p != wlog[k].lle && !n++) { first = (uint32_t)(uintptr_t)p; hv = *p; lv = wlog[k].lle; }
    }
    /* the native side's: only a byte's first write holds its value from before */
    for (size_t k = from; k < hlog_count; k++) {
        uint32_t a = (uint32_t)(uintptr_t)hlog[k].p;
        if (in_image(a)) {
            uint32_t o = a - IMAGE_BASE;
            if (seen[o >> 3] & (1 << (o & 7))) continue;
            seen[o >> 3] |= 1 << (o & 7);
        }
        if (*hlog[k].p != hlog[k].old && !n++) { first = a; hv = *hlog[k].p; lv = hlog[k].old; }
    }
    for (size_t k = 0; k < wlog_count; k++) {
        uint32_t o = (uint32_t)(uintptr_t)host_byte(wlog[k].addr) - IMAGE_BASE;
        seen[o >> 3] = 0;
    }
    for (size_t k = from; k < hlog_count; k++)
        if (in_image((uint32_t)(uintptr_t)hlog[k].p)) seen[((uint32_t)(uintptr_t)hlog[k].p - IMAGE_BASE) >> 3] = 0;
    if (n) report(names[w0 >> 24], cmd, w0, w1, "DRAM bytes", first, hv, lv, (unsigned)n);
    return n != 0;
}

/* Run `cmds` (at host address `at`) a command at a time on both, from the same DMEM, comparing each.
 * Returns the number of commands that differ. */
static unsigned check_commands(const OSTask *task, const uint32_t *at, const uint32_t *cmds, uint32_t words,
                               unsigned *bad_by_op, void (*before)(uint32_t *w0, uint32_t *w1)) {
    unsigned bad_cmds = 0;
    sync_dmem();
    for (uint32_t i = 0; i + 1 < words; i += 2) {
        uint32_t w0 = cmds[i], w1 = cmds[i + 1], pair[2];
        if (before) { before(&w0, &w1); sync_dmem_for(w0); }
        pair[0] = w0;
        pair[1] = w1;
        wlog_count = 0;
        logging = 1;
        run_list(task, at + i, pair, 2);
        logging = 0;
        undo_lle();
        size_t from = hlog_count;
        Port_AspWriteHook = note_host_write;
        Port_AspCommand(task, w0, w1);
        Port_AspWriteHook = NULL;
        int bad = compare_dram(i / 2, w0, w1, from);
        bad |= compare_dmem(i / 2, w0, w1);
        if (!before) hlog_count = from; /* the game's run keeps the native writes */
        if (bad) { bad_by_op[w0 >> 24]++; bad_cmds++; sync_dmem(); }
    }
    return bad_cmds;
}

/* ---- the fuzzer: a mutated copy of a real list, then everything undone ---- */

static uint32_t rnd(void) {
    fuzz_rng ^= fuzz_rng << 13;
    fuzz_rng ^= fuzz_rng >> 7;
    fuzz_rng ^= fuzz_rng << 17;
    return (uint32_t)(fuzz_rng >> 32);
}

/* a value that is often an edge: 0, +-1, the extremes, a power of two, or anything */
static s16 rnd16(void) {
    static const s16 edges[] = { 0, 1, -1, 0x7FFF, -0x8000, 0x7FFE, -0x7FFF, 0x4000, -0x4000, 0x0100 };
    uint32_t r = rnd();
    if ((r & 3) == 0) return edges[(r >> 2) % (sizeof edges / sizeof edges[0])];
    return (s16)(r >> 8);
}

static void fuzz_fill(u32 dram_addr, u32 bytes) {
    if (!dram_addr) return;
    u8 *p = (u8 *)(uintptr_t)(dram_addr < 0x00800000u ? (dram_addr | 0x80000000u) : dram_addr);
    note_host_write(p, bytes);
    for (u32 i = 0; i < bytes; i += 2) { s16 v = rnd16(); memcpy(p + i, &v, 2); }
}

/* full-scale samples over every buffer, given to the microcode as samples (a halfword that holds
 * ADPCM bytes would read differently, so this comes only before commands that read samples) */
static void fuzz_noise(void) {
    u8 *h = Port_AspDmem();
    for (uint32_t off = 0; off < BUF_BYTES; off += 2) { s16 v = rnd16(); memcpy(h + off, &v, 2); }
}

static uint32_t loop_addr;

/* a length changed by lo..hi bytes, mostly by whole samples */
static u16 fuzz_count(uint32_t w1, int lo, int hi) {
    s32 count = (s32)(w1 & 0xFFFF) + lo + (s32)(rnd() % (uint32_t)(hi - lo + 1));
    if (rnd() & 7) count &= ~1;
    return (u16)(count < 0 ? 0 : count);
}

/* SETBUFF lengths (the next command in view, for a bias: LOADBUFF mostly within its 8-byte DMA) */
static void fuzz_lengths(uint32_t *cmds, uint32_t words) {
    for (uint32_t i = 0; i + 3 < words; i += 2) {
        uint32_t op = cmds[i] >> 24, next = cmds[i + 2] >> 24;
        if (op != 8 || ((cmds[i] >> 16) & A_AUX) || (rnd() & 7)) continue;
        int lo = next == 4 && (rnd() & 3) ? -7 : -32, hi = next == 4 && (rnd() & 3) ? 0 : 32;
        cmds[i + 1] = (cmds[i + 1] & 0xFFFF0000u) | fuzz_count(cmds[i + 1], lo, hi);
    }
}

static void mutate(uint32_t *w0, uint32_t *w1) {
    uint32_t op = *w0 >> 24, r = rnd();
    switch (op) {
    case 1: /* ADPCM: flags, then a random state (and loop state) unless INIT */
        if (r & 1) *w0 ^= (uint32_t)A_INIT << 16;
        if (r & 2 && loop_addr) *w0 ^= (uint32_t)A_LOOP << 16; /* only while SETLOOP's address stands */
        if (!((*w0 >> 16) & A_INIT)) fuzz_fill(*w1, 32);
        if (((*w0 >> 16) & A_LOOP) && loop_addr) fuzz_fill(loop_addr, 32);
        break;
    case 3: /* ENVMIXER: INIT/AUX, a random state */
        if (r & 1) *w0 ^= (uint32_t)A_INIT << 16;
        if (r & 2) *w0 ^= (uint32_t)A_AUX << 16;
        if (!((*w0 >> 16) & A_INIT)) {
            fuzz_fill(*w1, 0x50);
            /* lanes the microcode clamps: keep each side's rate sign and target as the game's are
             * free, any value; but the state's own parameter block too */
        }
        break;
    case 5: /* RESAMPLE: pitch, INIT, a random state (position and the 4 samples) */
        if (r & 1) *w0 ^= (uint32_t)A_INIT << 16;
        *w0 = (*w0 & 0xFFFF0000u) | (rnd() & 0xFFFF);
        if (!((*w0 >> 16) & A_INIT)) fuzz_fill(*w1, 0x10);
        break;
    case 9: /* SETVOL: any volume, target, rate, dry and wet gain */
        *w0 = (*w0 & 0xFFFF0000u) | (u16)rnd16();
        if (!((*w0 >> 16) & A_VOL) || ((*w0 >> 16) & A_AUX)) *w1 = (r & 4) ? rnd() : (uint32_t)(u16)rnd16() << ((r & 8) ? 16 : 0);
        /* the left target and rate lie over SETLOOP's address (the microcode's parameter block) */
        if (!((*w0 >> 16) & (A_VOL | A_AUX)) && ((*w0 >> 16) & A_LEFT)) loop_addr = 0;
        break;
    case 12: *w0 = (*w0 & 0xFFFF0000u) | (u16)rnd16(); break; /* MIXER gain */
    case 14: /* POLEF: gain, INIT, a random history */
        *w0 = (*w0 & 0xFFFF0000u) | (u16)rnd16();
        if (r & 1) *w0 ^= (uint32_t)A_INIT << 16;
        if (!((*w0 >> 16) & A_INIT)) fuzz_fill(*w1, 8);
        break;
    case 15: loop_addr = *w1; break;
    case 2: /* CLEARBUFF: odd and longer or shorter lengths now and then */
        if ((r & 7) == 0) *w1 = (*w1 & 0xFFFF0000u) | fuzz_count(*w1, -32, 32);
        break;
    }
    if ((op == 1 || op == 3 || op == 5 || op == 6 || op == 12 || op == 13 || op == 14) && (rnd() & 3) == 0) fuzz_noise();
}

static void fuzz_task(const OSTask *task) {
    static uint32_t copy[0x8000 / 4];
    const uint32_t *list = (const uint32_t *)task->t.data_ptr;
    uint32_t words = task->t.data_size / 4;
    if (words > sizeof copy / 4) return;
    memcpy(copy, list, words * 4);
    fuzz_lengths(copy, words);
    uint8_t lle_dmem[0x1000];
    memcpy(lle_dmem, dmem, sizeof lle_dmem);
    Port_AspSaveState();
    Port_AspTaskStart(task);
    size_t from = hlog_count;
    loop_addr = 0;
    const char *was = phase;
    phase = "fuzz_task";
    fuzz_tasks++;
    fuzz_cmds += words / 2;
    check_commands(task, list, copy, words, fuzz_bad, mutate);
    phase = was;
    undo_host(from);
    Port_AspRestoreState();
    memcpy(dmem, lle_dmem, sizeof lle_dmem);
}

/* ---- the check, on every task ---- */

static void cover_list(const uint32_t *list, uint32_t words) {
    u16 count = 0;
    for (uint32_t i = 0; i + 1 < words; i += 2) {
        uint32_t w0 = list[i], op = w0 >> 24;
        cover[op][(w0 >> 16) & 0xFF]++;
        if (op == 8 && !((w0 >> 16) & A_AUX)) count = (u16)list[i + 1];
        if ((op == 1 || op == 5 || op == 3 || op == 12 || op == 14 || op == 4 || op == 6) && (count & 0x1F)) cover_odd[op]++;
    }
}

void Port_CheckAudioTask(const OSTask *task) {
    const uint32_t *list = (const uint32_t *)task->t.data_ptr;
    uint32_t words = task->t.data_size / 4;
    check_tasks++;
    load_ucode(task);
    if (fuzz_every && check_tasks % fuzz_every == 0) fuzz_task(task);
    cover_list(list, words);
    check_cmds += words / 2;
    if (mode == 1) {
        Port_AspTaskStart(task);
        if (check_commands(task, list, list, words, check_bad, NULL)) bad_tasks++;
        return;
    }
    /* whole task: the microcode from its own DMEM, undone; then the native code */
    static uint8_t lle_dmem[0x1000];
    if (check_tasks == 1) sync_dmem();
    memcpy(lle_dmem, dmem, sizeof lle_dmem);
    Port_AspSaveState();
    wlog_count = 0;
    logging = 1;
    uint64_t t0 = Port_HostNowNs();
    run_list(task, list, list, words);
    uint64_t t1 = Port_HostNowNs();
    logging = 0;
    undo_lle();
    hlog_count = 0;
    Port_AspWriteHook = note_host_write;
    Port_RunAudioTask(task);
    Port_AspWriteHook = NULL;
    time_lle_ns += t1 - t0;
    uint32_t last = words >= 2 ? list[words - 2] : 0;
    int bad = compare_dram(words / 2, last, 0, 0);
    bad |= compare_dmem(words / 2, last, 0);
    if (!bad) { hlog_count = 0; return; }
    /* name the command: run it again a command at a time from the same start */
    bad_tasks++;
    undo_host(0);
    Port_AspRestoreState();
    memcpy(dmem, lle_dmem, sizeof lle_dmem);
    phase = "task_start"; /* what the two started from (the commands below start from the same) */
    Port_AspTaskStart(task);
    compare_dmem(0, 0, 0);
    phase = "task_cmd";
    check_commands(task, list, list, words, check_bad, NULL);
    phase = "task";
    hlog_count = 0;
}

int Port_AudioRefInit(void) {
    const char *check = getenv("ELTALE_AUDIO_CHECK"), *fuzz = getenv("ELTALE_AUDIO_FUZZ");
    if (!check && !fuzz) return 0;
    mode = check && !strcmp(check, "task") ? 2 : 1;
    if (fuzz) fuzz_every = (unsigned)atoi(fuzz) ? (unsigned)atoi(fuzz) : 1;
    if (getenv("ELTALE_AUDIO_FUZZ_SEED")) fuzz_rng ^= strtoull(getenv("ELTALE_AUDIO_FUZZ_SEED"), NULL, 0) * 0x2545F4914F6CDD1Dull;
    if (!fuzz_rng) fuzz_rng = 1;
    return 1;
}

void Port_CheckAudioReport(void) {
    if (!check_tasks) return;
    unsigned total = 0;
    for (int i = 0; i < 16; i++) total += check_bad[i];
    fprintf(stderr, "P1_AUDIO_CHECK mode=%s tasks=%u commands=%u differing_tasks=%u differing_commands=%u",
            mode == 2 ? "task" : "cmd", check_tasks, check_cmds, bad_tasks, total);
    for (int i = 0; i < 16; i++) if (check_bad[i]) fprintf(stderr, " %s=%u", names[i], check_bad[i]);
    fprintf(stderr, "\n");
    if (fuzz_tasks) {
        unsigned fb = 0;
        for (int i = 0; i < 16; i++) fb += fuzz_bad[i];
        fprintf(stderr, "P1_AUDIO_FUZZ tasks=%u commands=%u differing_commands=%u", fuzz_tasks, fuzz_cmds, fb);
        for (int i = 0; i < 16; i++) if (fuzz_bad[i]) fprintf(stderr, " %s=%u", names[i], fuzz_bad[i]);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "P1_AUDIO_COVER");
    for (int op = 0; op < 16; op++)
        for (int f = 0; f < 256; f++)
            if (cover[op][f]) fprintf(stderr, " %s/%02x=%u", names[op], f, cover[op][f]);
    for (int op = 0; op < 16; op++)
        if (cover_odd[op]) fprintf(stderr, " %s:count%%32=%u", names[op], cover_odd[op]);
    fprintf(stderr, "\n");
    if (mode == 2)
        fprintf(stderr, "P1_AUDIO_TIME microcode tasks=%u us_per_task=%.1f\n", check_tasks,
                time_lle_ns / 1e3 / check_tasks);
}
