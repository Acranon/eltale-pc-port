/* The reference for port/audio/aspmain.c: the RSP interpreter (port/rsp) running the game's own
 * aspMain microcode (text and data from the user's ROM, in RDRAM) on an audio task.
 *
 * ELTALE_AUDIO_LLE=1   play with the microcode instead of the HLE.
 * ELTALE_AUDIO_CHECK=1 run both on every task and compare every byte the microcode writes; the
 *                      microcode's writes are undone, so the game hears the HLE.
 *
 * The microcode sees memory big-endian; the native game keeps it in host order. The DMA converts by
 * what the memory holds: sample data the PI copied from the ROM is bytes, the task's command list
 * is 32-bit words, and everything else the audio code hands the RSP (states, books, mix and output
 * buffers) is 16-bit samples. A DRAM address is 24 bits on the RSP; the audio heap and arenas sit in
 * the image at 0x10000000-0x10FFFFFF, so it maps back to the host by adding 0x10000000. */
#include "ultra64.h"
#include "port/audio.h"
#include "../rsp/rsp.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((noreturn)) void Port_HostFatal(const char *what);

/* ---- what host memory holds: ROM bytes (one bit per 8 bytes of the image's first 16 MiB) ---- */

#define IMAGE_BASE 0x10000000u
static uint8_t rom_bytes_map[(16u << 20) / 8 / 8];

void Port_LleNotePiDma(void *dst, u32 size) {
    uint32_t a = (uint32_t)(uintptr_t)dst;
    if (a < IMAGE_BASE || a - IMAGE_BASE + size > (16u << 20)) return;
    for (uint32_t b = (a - IMAGE_BASE) >> 3, e = (a - IMAGE_BASE + size + 7) >> 3; b < e; b++)
        rom_bytes_map[b >> 3] |= 1 << (b & 7);
}

/* the task's command list, with its DRAM addresses made 24-bit: aspMain takes the whole top byte as
 * the segment number, so 0x10xxxxxx would index past its segment table */
static uint32_t list_start, list_end;
static uint32_t list_copy[0x8000 / 4];

/* host byte address of big-endian byte `a` (a host address) */
static uint8_t *host_byte(uint32_t a) {
    if (a < IMAGE_BASE || a >= IMAGE_BASE + (16u << 20)) Port_HostFatal("RSP DMA outside the image");
    uint32_t o = a - IMAGE_BASE;
    if (rom_bytes_map[o >> 6] & (1 << ((o >> 3) & 7))) return (uint8_t *)(uintptr_t)a;
    if (a >= list_start && a < list_end) return (uint8_t *)list_copy + ((a - list_start) ^ 3);
    return (uint8_t *)(uintptr_t)(a ^ 1);
}

/* ---- writes, logged so the check mode can undo them and compare ---- */

typedef struct { uint32_t addr; uint8_t old, lle; } WriteLog;
static WriteLog *wlog;
static size_t wlog_count, wlog_cap;
static int logging;

static void host_write(uint32_t a, uint8_t v) {
    uint8_t *p = host_byte(a);
    if (logging) {
        if (wlog_count == wlog_cap) {
            wlog_cap = wlog_cap ? wlog_cap * 2 : 1 << 16;
            wlog = realloc(wlog, wlog_cap * sizeof *wlog);
            if (!wlog) Port_HostFatal("out of memory");
        }
        wlog[wlog_count++] = (WriteLog){ a, *p, v };
    }
    *p = v;
}

/* ---- the RSP (port/rsp), with DRAM at the image's 24-bit addresses ---- */

static Rsp rsp;
static uint8_t *const dmem = rsp.dmem;
static uint8_t *const imem = rsp.imem;

static uint8_t dram_read(void *ctx, uint32_t a) { (void)ctx; return *host_byte(IMAGE_BASE + a); }
static void dram_write(void *ctx, uint32_t a, uint8_t v) { (void)ctx; host_write(IMAGE_BASE + a, v); }

static void store(uint32_t a, int bytes, uint32_t v) {
    for (int i = bytes - 1; i >= 0; i--, v >>= 8) dmem[(a + i) & 0xFFF] = (uint8_t)v;
}

/* what rspboot leaves: the microcode text at IMEM 0x080, its data at DMEM 0 */
static void load_ucode(const OSTask *task) {
    memcpy(imem + 0x80, (const void *)(uintptr_t)((uint32_t)(uintptr_t)task->t.ucode | 0x80000000u), 0x1000 - 0x80);
    memcpy(dmem, (const void *)(uintptr_t)((uint32_t)(uintptr_t)task->t.ucode_data | 0x80000000u), task->t.ucode_data_size);
}

/* Run `words` command words at host address `list` (the task's list, or one command of it). */
static void run_list(const OSTask *task, const uint32_t *list, uint32_t words) {
    uint32_t tw[16] = { 0 };
    tw[0] = task->t.type;
    tw[1] = task->t.flags;
    tw[12] = (uint32_t)(uintptr_t)list - IMAGE_BASE; /* data_ptr, as a 24-bit RDRAM address */
    tw[13] = words * 4;
    for (int i = 0; i < 16; i++) store(0xFC0 + i * 4, 4, tw[i]);
    list_start = (uint32_t)(uintptr_t)list;
    list_end = list_start + words * 4;
    if (words * 4 > sizeof list_copy) Port_HostFatal("RSP: audio command list too long");
    for (uint32_t i = 0; i < words; i += 2) {
        uint32_t w0 = list[i], w1 = list[i + 1], op = w0 >> 24;
        /* ADPCM ENVMIXER LOADBUFF RESAMPLE SAVEBUFF LOADADPCM POLEF SETLOOP carry an address */
        if (((1u << op) & 0xC87A) && w1 >= IMAGE_BASE && w1 < IMAGE_BASE + (16u << 20)) w1 -= IMAGE_BASE;
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

void Port_RunAudioTaskLle(const OSTask *task) {
    load_ucode(task);
    run_list(task, (const uint32_t *)task->t.data_ptr, task->t.data_size / 4);
}

/* ---- the check: each command run by both, from the same DMEM and DRAM ----
 * The microcode's buffers start at DMEM 0x5C0 (its SETBUFF adds that); the HLE keeps them at the
 * command lists' own offsets, in host order. A halfword matches if it reads the same as a sample, or
 * holds the same two bytes (ADPCM data straight from the ROM). After each command the microcode's
 * DRAM writes are undone and its DMEM is set to the HLE's, so the game hears the HLE and each
 * command is checked from the same start. */
#define BUF_BASE 0x5C0
#define BUF_BYTES (0xFC0 - BUF_BASE)

static unsigned check_tasks, check_cmds, check_bad[16], check_printed;

static int dmem_same(const uint8_t *h, uint32_t off) {
    const uint8_t *l = dmem + BUF_BASE + off;
    return (h[off] == l[1] && h[off + 1] == l[0]) || (h[off] == l[0] && h[off + 1] == l[1]);
}

static void sync_dmem(void) {
    const uint8_t *h = Port_AspDmem();
    for (uint32_t off = 0; off < BUF_BYTES; off += 2)
        if (!dmem_same(h, off)) { dmem[BUF_BASE + off] = h[off + 1]; dmem[BUF_BASE + off + 1] = h[off]; }
}

static void report(uint32_t i, uint32_t w0, uint32_t w1, const char *what, uint32_t at, int hv, int lv, unsigned n) {
    if (check_printed++ >= 60) return;
    fprintf(stderr, "P1_AUDIO_CHECK task=%u cmd=%u %08x %08x: %s differ at %08x (%d / %d, %u halfwords)\n",
            check_tasks, i, w0, w1, what, at, hv, lv, n);
}

void Port_CheckAudioTask(const OSTask *task) {
    static uint8_t seen[(16u << 20) / 8];
    const uint32_t *list = (const uint32_t *)task->t.data_ptr;
    const uint8_t *h = Port_AspDmem();
    check_tasks++;
    load_ucode(task);
    sync_dmem();
    for (uint32_t i = 0; i < task->t.data_size / 4; i += 2) {
        uint32_t w0 = list[i], w1 = list[i + 1];
        wlog_count = 0;
        logging = 1;
        run_list(task, list + i, 2);
        logging = 0;
        for (size_t k = wlog_count; k-- > 0;) *host_byte(wlog[k].addr) = wlog[k].old;
        Port_AspCommand(task, w0, w1);
        check_cmds++;
        int bad = 0;
        /* DMEM buffers */
        unsigned n = 0;
        uint32_t first = 0;
        for (uint32_t off = 0; off < BUF_BYTES; off += 2)
            if (!dmem_same(h, off)) { if (!n++) first = off; }
        if (n) {
            bad = 1;
            report(i / 2, w0, w1, "DMEM", first, *(const int16_t *)(h + first),
                   (int16_t)(dmem[BUF_BASE + first] << 8 | dmem[BUF_BASE + first + 1]), n);
        }
        /* DRAM: the last value the microcode wrote to each byte, against what the HLE left */
        size_t dn = 0;
        uint32_t dfirst = 0;
        uint8_t dl = 0;
        for (size_t k = wlog_count; k-- > 0;) {
            uint32_t o = wlog[k].addr - IMAGE_BASE;
            if (seen[o >> 3] & (1 << (o & 7))) continue;
            seen[o >> 3] |= 1 << (o & 7);
            if (*host_byte(wlog[k].addr) != wlog[k].lle) { dn++; dfirst = wlog[k].addr; dl = wlog[k].lle; }
        }
        for (size_t k = 0; k < wlog_count; k++) seen[(wlog[k].addr - IMAGE_BASE) >> 3] = 0;
        if (dn) {
            bad = 1;
            report(i / 2, w0, w1, "DRAM bytes", dfirst, *host_byte(dfirst), dl, (unsigned)dn);
        }
        if (bad) { check_bad[w0 >> 24]++; sync_dmem(); }
    }
}

void Port_CheckAudioReport(void) {
    if (!check_tasks) return;
    static const char *names[16] = { "SPNOOP", "ADPCM", "CLEARBUFF", "ENVMIXER", "LOADBUFF", "RESAMPLE",
                                     "SAVEBUFF", "SEGMENT", "SETBUFF", "SETVOL", "DMEMMOVE", "LOADADPCM",
                                     "MIXER", "INTERLEAVE", "POLEF", "SETLOOP" };
    fprintf(stderr, "P1_AUDIO_CHECK tasks=%u commands=%u differing:", check_tasks, check_cmds);
    for (int i = 0; i < 16; i++) if (check_bad[i]) fprintf(stderr, " %s=%u", names[i], check_bad[i]);
    fprintf(stderr, "\n");
}
