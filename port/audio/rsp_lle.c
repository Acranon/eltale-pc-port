/* The reference for port/audio/aspmain.c: an RSP interpreter that runs the game's own aspMain
 * microcode (text and data from the user's ROM, in RDRAM) on an audio task.
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

/* ---- the RSP ---- */

static uint8_t dmem[0x1000], imem[0x1000];
static uint32_t r[32];
static uint32_t pc, npc, nnpc; /* this instruction, the next (a delay slot after a branch), the one after */
static int halted;
static uint32_t sp_mem_addr, sp_dram_addr;

typedef struct { uint16_t e[8]; } Vreg;
static Vreg vr[32];
static int64_t acc[8];
static uint16_t vco, vcc;
static uint8_t vce;

static uint8_t vbyte(const Vreg *v, int i) { i &= 15; return (i & 1) ? v->e[i >> 1] & 0xFF : v->e[i >> 1] >> 8; }
static void set_vbyte(Vreg *v, int i, uint8_t b) {
    i &= 15;
    v->e[i >> 1] = (i & 1) ? (v->e[i >> 1] & 0xFF00) | b : (v->e[i >> 1] & 0x00FF) | (b << 8);
}

static uint8_t dm8(uint32_t a) { return dmem[a & 0xFFF]; }
static void dm8w(uint32_t a, uint8_t v) { dmem[a & 0xFFF] = v; }

static void dma(int to_rdram, uint32_t len_reg) {
    uint32_t len = ((len_reg & 0xFFF) | 7) + 1, count = ((len_reg >> 12) & 0xFF) + 1, skip = (len_reg >> 20) & 0xFFF;
    uint32_t mem = sp_mem_addr & 0x1FF8, dram = sp_dram_addr & 0xFFFFF8;
    uint8_t *m = (mem & 0x1000) ? imem : dmem;
    for (uint32_t row = 0; row < count; row++) {
        for (uint32_t i = 0; i < len; i++) {
            uint32_t h = IMAGE_BASE + dram + i;
            if (to_rdram) host_write(h, m[(mem + i) & 0xFFF]);
            else m[(mem + i) & 0xFFF] = *host_byte(h);
        }
        mem = (mem & 0x1000) | ((mem + len) & 0xFFF);
        dram += len + skip;
    }
    sp_mem_addr = mem;
    sp_dram_addr = dram;
}

static uint32_t mfc0(int rd) {
    switch (rd) {
    case 0: return sp_mem_addr;
    case 1: return sp_dram_addr;
    case 4: return 0; /* SP_STATUS: no signals, so no yield */
    case 5: case 6: return 0; /* DMA full, busy */
    case 7: return 0; /* semaphore: always free */
    default: return 0;
    }
}

static void mtc0(int rd, uint32_t v) {
    switch (rd) {
    case 0: sp_mem_addr = v; break;
    case 1: sp_dram_addr = v; break;
    case 2: dma(0, v); break;
    case 3: dma(1, v); break;
    case 4: if (v & 2) halted = 1; break; /* SP_SET_HALT */
    default: break;
    }
}

/* element selection for vector computational ops */
static int sel(int e, int i) {
    if (e < 2) return i;
    if (e < 4) return (i & ~1) | (e & 1);
    if (e < 8) return (i & ~3) | (e & 3);
    return e & 7;
}

static int64_t sext48(int64_t a) { return (int64_t)((uint64_t)a << 16) >> 16; }
static uint16_t acc_h(int i) { return (uint16_t)(acc[i] >> 32); }
static uint16_t acc_m(int i) { return (uint16_t)(acc[i] >> 16); }
static uint16_t acc_l(int i) { return (uint16_t)acc[i]; }
static void set_acc_l(int i, uint16_t v) { acc[i] = sext48((acc[i] & ~(int64_t)0xFFFF) | v); }

/* ares' accumulatorSaturate */
static uint16_t saturate(int i, int mid, uint16_t neg, uint16_t pos) {
    int16_t h = (int16_t)acc_h(i), m = (int16_t)acc_m(i);
    if (h < 0) { if (h != -1 || m >= 0) return neg; }
    else if (h != 0 || m < 0) return pos;
    return mid ? acc_m(i) : acc_l(i);
}
static uint16_t sclamp16(int32_t v) { return v < -32768 ? 0x8000 : v > 32767 ? 0x7FFF : (uint16_t)v; }

static void vector_op(uint32_t op) {
    int e = (op >> 21) & 15, vt = (op >> 16) & 31, vs = (op >> 11) & 31, vd = (op >> 6) & 31;
    const Vreg s = vr[vs];
    Vreg t, d;
    for (int i = 0; i < 8; i++) t.e[i] = vr[vt].e[sel(e, i)];
    switch (op & 63) {
    case 0x00: case 0x01: case 0x08: case 0x09: /* VMULF VMULU VMACF VMACU */
        for (int i = 0; i < 8; i++) {
            int64_t p = (int64_t)(int16_t)s.e[i] * (int16_t)t.e[i] * 2;
            acc[i] = sext48((op & 8) ? acc[i] + p : p + 0x8000);
            d.e[i] = (op & 1) ? (((int16_t)acc_h(i) < 0) ? 0 : (acc_h(i) || (acc_m(i) & 0x8000)) ? 0xFFFF : acc_m(i))
                              : saturate(i, 1, 0x8000, 0x7FFF);
        }
        break;
    case 0x04: case 0x0C: /* VMUDL VMADL */
        for (int i = 0; i < 8; i++) {
            int64_t p = ((uint32_t)s.e[i] * t.e[i]) >> 16;
            acc[i] = sext48((op & 8) ? acc[i] + p : p);
            d.e[i] = (op & 8) ? saturate(i, 0, 0, 0xFFFF) : acc_l(i);
        }
        break;
    case 0x05: case 0x0D: /* VMUDM VMADM */
        for (int i = 0; i < 8; i++) {
            int64_t p = (int64_t)(int16_t)s.e[i] * t.e[i];
            acc[i] = sext48((op & 8) ? acc[i] + p : p);
            d.e[i] = (op & 8) ? saturate(i, 1, 0x8000, 0x7FFF) : acc_m(i);
        }
        break;
    case 0x06: case 0x0E: /* VMUDN VMADN */
        for (int i = 0; i < 8; i++) {
            int64_t p = (int64_t)s.e[i] * (int16_t)t.e[i];
            acc[i] = sext48((op & 8) ? acc[i] + p : p);
            d.e[i] = (op & 8) ? saturate(i, 0, 0, 0xFFFF) : acc_l(i);
        }
        break;
    case 0x07: case 0x0F: /* VMUDH VMADH */
        for (int i = 0; i < 8; i++) {
            int64_t p = (int64_t)((int64_t)(int16_t)s.e[i] * (int16_t)t.e[i]) * 65536;
            acc[i] = sext48((op & 8) ? acc[i] + p : p);
            d.e[i] = saturate(i, 1, 0x8000, 0x7FFF);
        }
        break;
    case 0x10: case 0x11: /* VADD VSUB */
        for (int i = 0; i < 8; i++) {
            int c = (vco >> i) & 1;
            int32_t v = (op & 1) ? (int16_t)s.e[i] - (int16_t)t.e[i] - c : (int16_t)s.e[i] + (int16_t)t.e[i] + c;
            set_acc_l(i, (uint16_t)v);
            d.e[i] = sclamp16(v);
        }
        vco = 0;
        break;
    case 0x13: /* VABS */
        for (int i = 0; i < 8; i++) {
            int16_t a = (int16_t)s.e[i];
            uint16_t v = a > 0 ? t.e[i] : a == 0 ? 0 : (uint16_t)-(int32_t)(int16_t)t.e[i];
            set_acc_l(i, v);
            d.e[i] = (a < 0 && t.e[i] == 0x8000) ? 0x7FFF : v;
        }
        break;
    case 0x14: /* VADDC */
        vco = 0;
        for (int i = 0; i < 8; i++) {
            uint32_t v = (uint32_t)s.e[i] + t.e[i];
            set_acc_l(i, (uint16_t)v);
            d.e[i] = (uint16_t)v;
            vco |= (v >> 16) << i;
        }
        break;
    case 0x15: /* VSUBC */
        vco = 0;
        for (int i = 0; i < 8; i++) {
            int32_t v = (int32_t)s.e[i] - t.e[i];
            set_acc_l(i, (uint16_t)v);
            d.e[i] = (uint16_t)v;
            vco |= (v < 0) << i | (v != 0) << (i + 8);
        }
        break;
    case 0x1D: /* VSAR */
        for (int i = 0; i < 8; i++) d.e[i] = e == 8 ? acc_h(i) : e == 9 ? acc_m(i) : e == 10 ? acc_l(i) : 0;
        break;
    case 0x20: case 0x21: case 0x22: case 0x23: { /* VLT VEQ VNE VGE */
        uint16_t cc = 0;
        for (int i = 0; i < 8; i++) {
            int16_t a = (int16_t)s.e[i], b = (int16_t)t.e[i];
            int ne = (vco >> (i + 8)) & 1, carry = (vco >> i) & 1, c;
            switch (op & 63) {
            case 0x20: c = a < b || (a == b && ne && carry); break;
            case 0x21: c = a == b && !ne; break;
            case 0x22: c = a != b || ne; break;
            default:   c = a > b || (a == b && !(ne && carry)); break;
            }
            d.e[i] = c ? s.e[i] : t.e[i];
            set_acc_l(i, d.e[i]);
            cc |= c << i;
        }
        vcc = cc;
        vco = 0;
        break;
    }
    case 0x24: /* VCL */
        for (int i = 0; i < 8; i++) {
            int carry = (vco >> i) & 1, ne = (vco >> (i + 8)) & 1;
            int le = (vcc >> i) & 1, ge = (vcc >> (i + 8)) & 1;
            uint16_t a = s.e[i], b = t.e[i];
            if (carry) {
                if (!ne) {
                    uint32_t sum = (uint32_t)a + b;
                    int zero = (sum & 0xFFFF) == 0, over = sum > 0xFFFF;
                    le = ((vce >> i) & 1) ? (zero || over) : (zero && over);
                }
                d.e[i] = le ? (uint16_t)-b : a;
            } else {
                if (!ne) ge = (int32_t)a - (int32_t)b >= 0;
                d.e[i] = ge ? b : a;
            }
            vcc = (vcc & ~(0x101 << i)) | le << i | ge << (i + 8);
            set_acc_l(i, d.e[i]);
        }
        vco = 0;
        vce = 0;
        break;
    case 0x25: case 0x26: /* VCH VCR */
        vco = vcc = 0;
        vce = 0;
        for (int i = 0; i < 8; i++) {
            int16_t a = (int16_t)s.e[i], b = (int16_t)t.e[i];
            int le, ge, carry = 0, ne = 0, ce = 0;
            if ((a ^ b) < 0) {
                int32_t sum = (int32_t)a + b;
                ge = b < 0;
                if ((op & 63) == 0x25) {
                    le = sum <= 0;
                    ce = sum == -1;
                    ne = sum != 0 && sum != -1;
                    carry = 1;
                    d.e[i] = le ? (uint16_t)-b : (uint16_t)a;
                } else {
                    le = sum + 1 <= 0;
                    d.e[i] = le ? (uint16_t)~b : (uint16_t)a;
                }
            } else {
                int32_t diff = (int32_t)a - b;
                le = b < 0;
                ge = diff >= 0;
                if ((op & 63) == 0x25) ne = diff != 0;
                d.e[i] = ge ? (uint16_t)b : (uint16_t)a;
            }
            vcc |= le << i | ge << (i + 8);
            if ((op & 63) == 0x25) { vco |= carry << i | ne << (i + 8); vce |= ce << i; }
            set_acc_l(i, d.e[i]);
        }
        break;
    case 0x27: /* VMRG */
        for (int i = 0; i < 8; i++) { d.e[i] = ((vcc >> i) & 1) ? s.e[i] : t.e[i]; set_acc_l(i, d.e[i]); }
        vco = 0;
        break;
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D:
        for (int i = 0; i < 8; i++) {
            uint16_t a = s.e[i], b = t.e[i], v;
            switch (op & 63) {
            case 0x28: v = a & b; break;
            case 0x29: v = ~(a & b); break;
            case 0x2A: v = a | b; break;
            case 0x2B: v = ~(a | b); break;
            case 0x2C: v = a ^ b; break;
            default:   v = ~(a ^ b); break;
            }
            d.e[i] = v;
            set_acc_l(i, v);
        }
        break;
    case 0x33: /* VMOV */
        for (int i = 0; i < 8; i++) set_acc_l(i, t.e[i]);
        vr[vd].e[vs & 7] = t.e[vs & 7];
        return;
    case 0x37: case 0x3F: return; /* VNOP VNULL */
    default: {
        char msg[64];
        snprintf(msg, sizeof msg, "RSP: vector op %02x not implemented", op & 63);
        Port_HostFatal(msg);
    }
    }
    vr[vd] = d;
}

static void lwc2(uint32_t op) {
    int base = (op >> 21) & 31, vt = (op >> 16) & 31, kind = (op >> 11) & 31, e = (op >> 7) & 15;
    int32_t off = (int32_t)(op << 25) >> 25;
    Vreg *v = &vr[vt];
    static const int size[12] = { 1, 2, 4, 8, 16, 16, 8, 8, 16, 16, 16, 16 };
    if (kind > 11) Port_HostFatal("RSP: bad LWC2");
    uint32_t a = r[base] + off * size[kind];
    switch (kind) {
    case 0: case 1: case 2: case 3: /* LBV LSV LLV LDV */
        for (int i = e; i < e + size[kind] && i < 16; i++) set_vbyte(v, i, dm8(a++));
        break;
    case 4: { /* LQV */
        int end = e + (16 - (a & 15));
        for (int i = e; i < end && i < 16; i++) set_vbyte(v, i, dm8(a++));
        break;
    }
    case 5: { /* LRV */
        int start = 16 - ((a & 15) - e);
        a &= ~15u;
        for (int i = start; i < 16; i++) set_vbyte(v, i, dm8(a++));
        break;
    }
    case 6: case 7: { /* LPV LUV */
        int index = (a & 7) - e;
        a &= ~7u;
        for (int i = 0; i < 8; i++) v->e[i] = dm8(a + ((index + i) & 15)) << (kind == 6 ? 8 : 7);
        break;
    }
    case 8: { /* LHV */
        int index = (a & 7) - e;
        a &= ~7u;
        for (int i = 0; i < 8; i++) v->e[i] = dm8(a + ((index + i * 2) & 15)) << 7;
        break;
    }
    default: Port_HostFatal("RSP: LFV/LWV/LTV not implemented");
    }
}

static void swc2(uint32_t op) {
    int base = (op >> 21) & 31, vt = (op >> 16) & 31, kind = (op >> 11) & 31, e = (op >> 7) & 15;
    int32_t off = (int32_t)(op << 25) >> 25;
    const Vreg *v = &vr[vt];
    static const int size[12] = { 1, 2, 4, 8, 16, 16, 8, 8, 16, 16, 16, 16 };
    if (kind > 11) Port_HostFatal("RSP: bad SWC2");
    uint32_t a = r[base] + off * size[kind];
    switch (kind) {
    case 0: case 1: case 2: case 3: /* SBV SSV SLV SDV */
        for (int i = e; i < e + size[kind]; i++) dm8w(a++, vbyte(v, i));
        break;
    case 4: { /* SQV */
        int end = e + (16 - (a & 15));
        for (int i = e; i < end; i++) dm8w(a++, vbyte(v, i));
        break;
    }
    case 5: { /* SRV */
        int end = e + (a & 15), shift = 16 - (a & 15);
        a &= ~15u;
        for (int i = e; i < end; i++) dm8w(a++, vbyte(v, i + shift));
        break;
    }
    case 6: /* SPV */
        for (int i = e; i < e + 8; i++)
            dm8w(a++, (i & 15) < 8 ? vbyte(v, (i & 7) << 1) : (uint8_t)(v->e[i & 7] >> 7));
        break;
    case 7: /* SUV */
        for (int i = e; i < e + 8; i++)
            dm8w(a++, (i & 15) < 8 ? (uint8_t)(v->e[i & 7] >> 7) : vbyte(v, (i & 7) << 1));
        break;
    case 8: { /* SHV */
        int index = a & 7;
        a &= ~7u;
        for (int i = 0; i < 8; i++) {
            int b = e + i * 2;
            dm8w(a + ((index + i * 2) & 15), (uint8_t)(vbyte(v, b) << 1 | vbyte(v, b + 1) >> 7));
        }
        break;
    }
    default: Port_HostFatal("RSP: SFV/SWV/STV not implemented");
    }
}

static void branch(int taken, int32_t off) { if (taken) nnpc = (pc + 4 + (off << 2)) & 0xFFC; }

static uint32_t load(uint32_t a, int bytes, int sign) {
    uint32_t v = 0;
    for (int i = 0; i < bytes; i++) v = v << 8 | dm8(a + i);
    if (sign && bytes < 4) v = (uint32_t)((int32_t)(v << (32 - bytes * 8)) >> (32 - bytes * 8));
    return v;
}
static void store(uint32_t a, int bytes, uint32_t v) {
    for (int i = bytes - 1; i >= 0; i--, v >>= 8) dm8w(a + i, (uint8_t)v);
}

static void step(void) {
    uint32_t op = (uint32_t)imem[pc] << 24 | imem[pc + 1] << 16 | imem[pc + 2] << 8 | imem[pc + 3];
    uint32_t cur = pc;
    nnpc = (npc + 4) & 0xFFC;
    uint32_t rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, sa = (op >> 6) & 31;
    int32_t simm = (int16_t)op;
    uint32_t uimm = op & 0xFFFF;
    uint32_t res;
    int wr = -1;
    switch (op >> 26) {
    case 0x00:
        switch (op & 63) {
        case 0x00: res = r[rt] << sa; wr = rd; break;
        case 0x02: res = r[rt] >> sa; wr = rd; break;
        case 0x03: res = (uint32_t)((int32_t)r[rt] >> sa); wr = rd; break;
        case 0x04: res = r[rt] << (r[rs] & 31); wr = rd; break;
        case 0x06: res = r[rt] >> (r[rs] & 31); wr = rd; break;
        case 0x07: res = (uint32_t)((int32_t)r[rt] >> (r[rs] & 31)); wr = rd; break;
        case 0x08: nnpc = r[rs] & 0xFFC; break;
        case 0x09: res = (cur + 8) & 0xFFC; wr = rd; nnpc = r[rs] & 0xFFC; break;
        case 0x0D: halted = 1; break; /* BREAK */
        case 0x20: case 0x21: res = r[rs] + r[rt]; wr = rd; break;
        case 0x22: case 0x23: res = r[rs] - r[rt]; wr = rd; break;
        case 0x24: res = r[rs] & r[rt]; wr = rd; break;
        case 0x25: res = r[rs] | r[rt]; wr = rd; break;
        case 0x26: res = r[rs] ^ r[rt]; wr = rd; break;
        case 0x27: res = ~(r[rs] | r[rt]); wr = rd; break;
        case 0x2A: res = (int32_t)r[rs] < (int32_t)r[rt]; wr = rd; break;
        case 0x2B: res = r[rs] < r[rt]; wr = rd; break;
        default: goto bad;
        }
        break;
    case 0x01:
        switch (rt) {
        case 0x00: branch((int32_t)r[rs] < 0, simm); break;
        case 0x01: branch((int32_t)r[rs] >= 0, simm); break;
        case 0x10: { int t = (int32_t)r[rs] < 0; r[31] = (cur + 8) & 0xFFC; branch(t, simm); break; }
        case 0x11: { int t = (int32_t)r[rs] >= 0; r[31] = (cur + 8) & 0xFFC; branch(t, simm); break; }
        default: goto bad;
        }
        break;
    case 0x02: nnpc = (op << 2) & 0xFFC; break;
    case 0x03: r[31] = (cur + 8) & 0xFFC; nnpc = (op << 2) & 0xFFC; break;
    case 0x04: branch(r[rs] == r[rt], simm); break;
    case 0x05: branch(r[rs] != r[rt], simm); break;
    case 0x06: branch((int32_t)r[rs] <= 0, simm); break;
    case 0x07: branch((int32_t)r[rs] > 0, simm); break;
    case 0x08: case 0x09: res = r[rs] + (uint32_t)simm; wr = rt; break;
    case 0x0A: res = (int32_t)r[rs] < simm; wr = rt; break;
    case 0x0B: res = r[rs] < (uint32_t)simm; wr = rt; break;
    case 0x0C: res = r[rs] & uimm; wr = rt; break;
    case 0x0D: res = r[rs] | uimm; wr = rt; break;
    case 0x0E: res = r[rs] ^ uimm; wr = rt; break;
    case 0x0F: res = uimm << 16; wr = rt; break;
    case 0x10:
        if (rs == 0) { res = mfc0(rd); wr = rt; }
        else if (rs == 4) mtc0(rd, r[rt]);
        else goto bad;
        break;
    case 0x12:
        if (op & (1u << 25)) { vector_op(op); break; }
        switch (rs) {
        case 0x00: { int e = (op >> 7) & 15; res = (uint32_t)(int16_t)(vbyte(&vr[rd], e) << 8 | vbyte(&vr[rd], e + 1)); wr = rt; break; }
        case 0x02:
            res = rd == 0 ? vco : rd == 1 ? vcc : vce;
            res = rd == 2 ? res : (uint32_t)(int16_t)res;
            wr = rt;
            break;
        case 0x04: {
            int e = (op >> 7) & 15;
            set_vbyte(&vr[rd], e, (uint8_t)(r[rt] >> 8));
            if (e + 1 < 16) set_vbyte(&vr[rd], e + 1, (uint8_t)r[rt]);
            break;
        }
        case 0x06:
            if (rd == 0) vco = (uint16_t)r[rt];
            else if (rd == 1) vcc = (uint16_t)r[rt];
            else vce = (uint8_t)r[rt];
            break;
        default: goto bad;
        }
        break;
    case 0x20: res = load(r[rs] + simm, 1, 1); wr = rt; break;
    case 0x21: res = load(r[rs] + simm, 2, 1); wr = rt; break;
    case 0x23: res = load(r[rs] + simm, 4, 0); wr = rt; break;
    case 0x24: res = load(r[rs] + simm, 1, 0); wr = rt; break;
    case 0x25: res = load(r[rs] + simm, 2, 0); wr = rt; break;
    case 0x28: store(r[rs] + simm, 1, r[rt]); break;
    case 0x29: store(r[rs] + simm, 2, r[rt]); break;
    case 0x2B: store(r[rs] + simm, 4, r[rt]); break;
    case 0x32: lwc2(op); break;
    case 0x3A: swc2(op); break;
    default: goto bad;
    }
    if (wr > 0) r[wr] = res;
    pc = npc;
    npc = nnpc;
    return;
bad: {
    char msg[64];
    snprintf(msg, sizeof msg, "RSP: instruction %08x at %03x not implemented", op, cur);
    Port_HostFatal(msg);
}
}

static uint32_t pc_hist[32];

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
    memset(r, 0, sizeof r);
    r[1] = 0xFC0; /* rspboot leaves the task's DMEM address in $1 */
    halted = 0;
    pc = 0x80;
    npc = 0x84;
    for (uint64_t n = 0; !halted; n++) {
        if (n > 50000000) {
            for (int i = 0; i < 32; i++) {
                uint32_t h = pc_hist[(n + i) & 31];
                fprintf(stderr, "  pc %03x op %02x%02x%02x%02x\n", h, imem[h], imem[h + 1], imem[h + 2], imem[h + 3]);
            }
            Port_HostFatal("RSP: aspMain did not finish");
        }
        pc_hist[n & 31] = pc;
        step();
    }
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
