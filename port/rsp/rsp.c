/* The RSP interpreter: see rsp.h. Moved from port/audio/rsp_lle.c (2026-10-31) and given a memory
 * and RDP interface so the graphics reference can run F3DEX on it too. The vector rules follow ares
 * (accumulator saturation, VCL/VCH/VCR flags, VRCP/VRSQ tables). */
#include "rsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fatal(Rsp *rsp, const char *what) {
    if (rsp->fatal) rsp->fatal(what);
    fprintf(stderr, "%s\n", what);
    abort();
}

static uint8_t vbyte(const RspVreg *v, int i) { i &= 15; return (i & 1) ? v->e[i >> 1] & 0xFF : v->e[i >> 1] >> 8; }
static void set_vbyte(RspVreg *v, int i, uint8_t b) {
    i &= 15;
    v->e[i >> 1] = (i & 1) ? (v->e[i >> 1] & 0xFF00) | b : (v->e[i >> 1] & 0x00FF) | (b << 8);
}

#define dm8(a) (rsp->dmem[(a) & 0xFFF])
#define dm8w(a, v) (rsp->dmem[(a) & 0xFFF] = (uint8_t)(v))

static void dma(Rsp *rsp, int to_rdram, uint32_t len_reg) {
    uint32_t len = ((len_reg & 0xFFF) | 7) + 1, count = ((len_reg >> 12) & 0xFF) + 1, skip = (len_reg >> 20) & 0xFFF;
    uint32_t mem = rsp->sp_mem_addr & 0x1FF8, dram = rsp->sp_dram_addr & 0xFFFFF8;
    uint8_t *m = (mem & 0x1000) ? rsp->imem : rsp->dmem;
    for (uint32_t row = 0; row < count; row++) {
        for (uint32_t i = 0; i < len; i++) {
            if (to_rdram) rsp->dram_write(rsp->ctx, dram + i, m[(mem + i) & 0xFFF]);
            else m[(mem + i) & 0xFFF] = rsp->dram_read(rsp->ctx, dram + i);
        }
        mem = (mem & 0x1000) | ((mem + len) & 0xFFF);
        dram += len + skip;
    }
    rsp->sp_mem_addr = mem;
    rsp->sp_dram_addr = dram;
}

static uint32_t mfc0(Rsp *rsp, int rd) {
    switch (rd) {
    case 0: return rsp->sp_mem_addr;
    case 1: return rsp->sp_dram_addr;
    case 4: return 0; /* SP_STATUS: no signals, so no yield */
    case 5: case 6: return 0; /* DMA full, busy */
    case 7: return 0; /* semaphore: always free */
    default:
        if (rd >= 8 && rd < 16 && rsp->dpc_read) return rsp->dpc_read(rsp->ctx, rd - 8);
        return 0;
    }
}

static void mtc0(Rsp *rsp, int rd, uint32_t v) {
    switch (rd) {
    case 0: rsp->sp_mem_addr = v; break;
    case 1: rsp->sp_dram_addr = v; break;
    case 2: dma(rsp, 0, v); break;
    case 3: dma(rsp, 1, v); break;
    case 4: /* SP_STATUS */
        if (v & 2) rsp->halted = 1; /* SP_SET_HALT */
        for (int s = 0; s < 8; s++) {
            if (v & (1u << (9 + s * 2))) rsp->sp_status &= ~(1u << (7 + s)); /* CLR_SIGn */
            if (v & (1u << (10 + s * 2))) rsp->sp_status |= 1u << (7 + s);   /* SET_SIGn */
        }
        break;
    default:
        if (rd >= 8 && rd < 16 && rsp->dpc_write) rsp->dpc_write(rsp->ctx, rd - 8, v);
        break;
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
#define acc_h(i) ((uint16_t)(rsp->acc[i] >> 32))
#define acc_m(i) ((uint16_t)(rsp->acc[i] >> 16))
#define acc_l(i) ((uint16_t)rsp->acc[i])
static void set_acc_l(Rsp *rsp, int i, uint16_t v) { rsp->acc[i] = sext48((rsp->acc[i] & ~(int64_t)0xFFFF) | v); }

/* ares' accumulatorSaturate */
static uint16_t saturate(Rsp *rsp, int i, int mid, uint16_t neg, uint16_t pos) {
    int16_t h = (int16_t)acc_h(i), m = (int16_t)acc_m(i);
    if (h < 0) { if (h != -1 || m >= 0) return neg; }
    else if (h != 0 || m < 0) return pos;
    return mid ? acc_m(i) : acc_l(i);
}
static uint16_t sclamp16(int32_t v) { return v < -32768 ? 0x8000 : v > 32767 ? 0x7FFF : (uint16_t)v; }

/* ares' reciprocal and inverse square root tables */
static uint16_t rcp_table[512], rsq_table[512];
static int tables_ready;
static void init_tables(void) {
    for (uint32_t i = 0; i < 512; i++) {
        uint64_t a = i + 512, b = ((uint64_t)1 << 34) / a;
        rcp_table[i] = (uint16_t)((b + 1) >> 8);
        a = (uint64_t)(i + 512) >> (i & 1);
        b = (uint64_t)1 << 17;
        while (a * (b + 1) * (b + 1) < ((uint64_t)1 << 44)) b++;
        rsq_table[i] = (uint16_t)(b >> 1);
    }
    tables_ready = 1;
}

/* VRCP, VRCPL, VRSQ, VRSQL */
static int32_t divide(Rsp *rsp, int16_t in16, int low, int sqrt) {
    int32_t input = low && rsp->div_dp ? (int32_t)((uint32_t)(uint16_t)rsp->div_in << 16 | (uint16_t)in16) : in16;
    int32_t mask = input >> 31, data = input ^ mask, result;
    if (input > -32768) data -= mask;
    if (data == 0) result = 0x7FFFFFFF;
    else if (input == -32768) result = (int32_t)0xFFFF0000;
    else {
        uint32_t shift = (uint32_t)__builtin_clz((uint32_t)data);
        uint32_t index = (uint32_t)(((uint64_t)(uint32_t)data << shift & 0x7FC00000) >> 22);
        if (sqrt) {
            result = rsq_table[(index & 0x1FE) | (shift & 1)];
            result = (int32_t)((0x10000u | (uint32_t)result) << 14);
            result = (int32_t)(((uint32_t)result >> ((31 - shift) >> 1)) ^ (uint32_t)mask);
        } else {
            result = rcp_table[index];
            result = (int32_t)((0x10000u | (uint32_t)result) << 14);
            result = (int32_t)(((uint32_t)result >> (31 - shift)) ^ (uint32_t)mask);
        }
    }
    rsp->div_dp = 0;
    rsp->div_out = (int16_t)(result >> 16);
    return result;
}

static void vector_op(Rsp *rsp, uint32_t op) {
    int e = (op >> 21) & 15, vt = (op >> 16) & 31, vs = (op >> 11) & 31, vd = (op >> 6) & 31;
    const RspVreg s = rsp->vr[vs];
    RspVreg t, d;
    for (int i = 0; i < 8; i++) t.e[i] = rsp->vr[vt].e[sel(e, i)];
    switch (op & 63) {
    case 0x00: case 0x01: case 0x08: case 0x09: /* VMULF VMULU VMACF VMACU */
        for (int i = 0; i < 8; i++) {
            int64_t p = (int64_t)(int16_t)s.e[i] * (int16_t)t.e[i] * 2;
            rsp->acc[i] = sext48((op & 8) ? rsp->acc[i] + p : p + 0x8000);
            d.e[i] = (op & 1) ? (((int16_t)acc_h(i) < 0) ? 0 : (acc_h(i) || (acc_m(i) & 0x8000)) ? 0xFFFF : acc_m(i))
                              : saturate(rsp, i, 1, 0x8000, 0x7FFF);
        }
        break;
    case 0x04: case 0x0C: /* VMUDL VMADL */
        for (int i = 0; i < 8; i++) {
            int64_t p = ((uint32_t)s.e[i] * t.e[i]) >> 16;
            rsp->acc[i] = sext48((op & 8) ? rsp->acc[i] + p : p);
            d.e[i] = (op & 8) ? saturate(rsp, i, 0, 0, 0xFFFF) : acc_l(i);
        }
        break;
    case 0x05: case 0x0D: /* VMUDM VMADM */
        for (int i = 0; i < 8; i++) {
            int64_t p = (int64_t)(int16_t)s.e[i] * t.e[i];
            rsp->acc[i] = sext48((op & 8) ? rsp->acc[i] + p : p);
            d.e[i] = (op & 8) ? saturate(rsp, i, 1, 0x8000, 0x7FFF) : acc_m(i);
        }
        break;
    case 0x06: case 0x0E: /* VMUDN VMADN */
        for (int i = 0; i < 8; i++) {
            int64_t p = (int64_t)s.e[i] * (int16_t)t.e[i];
            rsp->acc[i] = sext48((op & 8) ? rsp->acc[i] + p : p);
            d.e[i] = (op & 8) ? saturate(rsp, i, 0, 0, 0xFFFF) : acc_l(i);
        }
        break;
    case 0x07: case 0x0F: /* VMUDH VMADH */
        for (int i = 0; i < 8; i++) {
            int64_t p = (int64_t)((int64_t)(int16_t)s.e[i] * (int16_t)t.e[i]) * 65536;
            rsp->acc[i] = sext48((op & 8) ? rsp->acc[i] + p : p);
            d.e[i] = saturate(rsp, i, 1, 0x8000, 0x7FFF);
        }
        break;
    case 0x10: case 0x11: /* VADD VSUB */
        for (int i = 0; i < 8; i++) {
            int c = (rsp->vco >> i) & 1;
            int32_t v = (op & 1) ? (int16_t)s.e[i] - (int16_t)t.e[i] - c : (int16_t)s.e[i] + (int16_t)t.e[i] + c;
            set_acc_l(rsp, i, (uint16_t)v);
            d.e[i] = sclamp16(v);
        }
        rsp->vco = 0;
        break;
    case 0x13: /* VABS */
        for (int i = 0; i < 8; i++) {
            int16_t a = (int16_t)s.e[i];
            uint16_t v = a > 0 ? t.e[i] : a == 0 ? 0 : (uint16_t)-(int32_t)(int16_t)t.e[i];
            set_acc_l(rsp, i, v);
            d.e[i] = (a < 0 && t.e[i] == 0x8000) ? 0x7FFF : v;
        }
        break;
    case 0x14: /* VADDC */
        rsp->vco = 0;
        for (int i = 0; i < 8; i++) {
            uint32_t v = (uint32_t)s.e[i] + t.e[i];
            set_acc_l(rsp, i, (uint16_t)v);
            d.e[i] = (uint16_t)v;
            rsp->vco |= (v >> 16) << i;
        }
        break;
    case 0x15: /* VSUBC */
        rsp->vco = 0;
        for (int i = 0; i < 8; i++) {
            int32_t v = (int32_t)s.e[i] - t.e[i];
            set_acc_l(rsp, i, (uint16_t)v);
            d.e[i] = (uint16_t)v;
            rsp->vco |= (v < 0) << i | (v != 0) << (i + 8);
        }
        break;
    case 0x1D: /* VSAR */
        for (int i = 0; i < 8; i++) d.e[i] = e == 8 ? acc_h(i) : e == 9 ? acc_m(i) : e == 10 ? acc_l(i) : 0;
        break;
    case 0x20: case 0x21: case 0x22: case 0x23: { /* VLT VEQ VNE VGE */
        uint16_t cc = 0;
        for (int i = 0; i < 8; i++) {
            int16_t a = (int16_t)s.e[i], b = (int16_t)t.e[i];
            int ne = (rsp->vco >> (i + 8)) & 1, carry = (rsp->vco >> i) & 1, c;
            switch (op & 63) {
            case 0x20: c = a < b || (a == b && ne && carry); break;
            case 0x21: c = a == b && !ne; break;
            case 0x22: c = a != b || ne; break;
            default:   c = a > b || (a == b && !(ne && carry)); break;
            }
            d.e[i] = c ? s.e[i] : t.e[i];
            set_acc_l(rsp, i, d.e[i]);
            cc |= c << i;
        }
        rsp->vcc = cc;
        rsp->vco = 0;
        break;
    }
    case 0x24: /* VCL */
        for (int i = 0; i < 8; i++) {
            int carry = (rsp->vco >> i) & 1, ne = (rsp->vco >> (i + 8)) & 1;
            int le = (rsp->vcc >> i) & 1, ge = (rsp->vcc >> (i + 8)) & 1;
            uint16_t a = s.e[i], b = t.e[i];
            if (carry) {
                if (!ne) {
                    uint32_t sum = (uint32_t)a + b;
                    int zero = (sum & 0xFFFF) == 0, over = sum > 0xFFFF;
                    le = ((rsp->vce >> i) & 1) ? (zero || over) : (zero && over);
                }
                d.e[i] = le ? (uint16_t)-b : a;
            } else {
                if (!ne) ge = (int32_t)a - (int32_t)b >= 0;
                d.e[i] = ge ? b : a;
            }
            rsp->vcc = (rsp->vcc & ~(0x101 << i)) | le << i | ge << (i + 8);
            set_acc_l(rsp, i, d.e[i]);
        }
        rsp->vco = 0;
        rsp->vce = 0;
        break;
    case 0x25: case 0x26: /* VCH VCR */
        rsp->vco = rsp->vcc = 0;
        rsp->vce = 0;
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
            rsp->vcc |= le << i | ge << (i + 8);
            if ((op & 63) == 0x25) { rsp->vco |= carry << i | ne << (i + 8); rsp->vce |= ce << i; }
            set_acc_l(rsp, i, d.e[i]);
        }
        break;
    case 0x27: /* VMRG */
        for (int i = 0; i < 8; i++) { d.e[i] = ((rsp->vcc >> i) & 1) ? s.e[i] : t.e[i]; set_acc_l(rsp, i, d.e[i]); }
        rsp->vco = 0;
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
            set_acc_l(rsp, i, v);
        }
        break;
    case 0x30: case 0x31: case 0x34: case 0x35: { /* VRCP VRCPL VRSQ VRSQL */
        if (!tables_ready) init_tables();
        int32_t res = divide(rsp, (int16_t)rsp->vr[vt].e[e & 7], (op & 63) == 0x31 || (op & 63) == 0x35,
                             (op & 63) >= 0x34);
        for (int i = 0; i < 8; i++) set_acc_l(rsp, i, t.e[i]);
        rsp->vr[vd].e[vs & 7] = (uint16_t)res;
        return;
    }
    case 0x32: case 0x36: /* VRCPH VRSQH */
        for (int i = 0; i < 8; i++) set_acc_l(rsp, i, t.e[i]);
        rsp->div_dp = 1;
        rsp->div_in = (int16_t)rsp->vr[vt].e[e & 7];
        rsp->vr[vd].e[vs & 7] = (uint16_t)rsp->div_out;
        return;
    case 0x33: /* VMOV */
        for (int i = 0; i < 8; i++) set_acc_l(rsp, i, t.e[i]);
        rsp->vr[vd].e[vs & 7] = t.e[vs & 7];
        return;
    case 0x37: case 0x3F: return; /* VNOP VNULL */
    default: {
        char msg[64];
        snprintf(msg, sizeof msg, "RSP: vector op %02x not implemented", op & 63);
        fatal(rsp, msg);
    }
    }
    rsp->vr[vd] = d;
}

static void lwc2(Rsp *rsp, uint32_t op) {
    int base = (op >> 21) & 31, vt = (op >> 16) & 31, kind = (op >> 11) & 31, e = (op >> 7) & 15;
    int32_t off = (int32_t)(op << 25) >> 25;
    RspVreg *v = &rsp->vr[vt];
    static const int size[12] = { 1, 2, 4, 8, 16, 16, 8, 8, 16, 16, 16, 16 };
    if (kind > 11) fatal(rsp, "RSP: bad LWC2");
    uint32_t a = rsp->r[base] + off * size[kind];
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
    default: fatal(rsp, "RSP: LFV/LWV/LTV not implemented");
    }
}

static void swc2(Rsp *rsp, uint32_t op) {
    int base = (op >> 21) & 31, vt = (op >> 16) & 31, kind = (op >> 11) & 31, e = (op >> 7) & 15;
    int32_t off = (int32_t)(op << 25) >> 25;
    const RspVreg *v = &rsp->vr[vt];
    static const int size[12] = { 1, 2, 4, 8, 16, 16, 8, 8, 16, 16, 16, 16 };
    if (kind > 11) fatal(rsp, "RSP: bad SWC2");
    uint32_t a = rsp->r[base] + off * size[kind];
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
    default: fatal(rsp, "RSP: SFV/SWV/STV not implemented");
    }
}

static void branch(Rsp *rsp, int taken, int32_t off) { if (taken) rsp->nnpc = (rsp->pc + 4 + (off << 2)) & 0xFFC; }

static uint32_t load(Rsp *rsp, uint32_t a, int bytes, int sign) {
    uint32_t v = 0;
    for (int i = 0; i < bytes; i++) v = v << 8 | dm8(a + i);
    if (sign && bytes < 4) v = (uint32_t)((int32_t)(v << (32 - bytes * 8)) >> (32 - bytes * 8));
    return v;
}
static void store(Rsp *rsp, uint32_t a, int bytes, uint32_t v) {
    for (int i = bytes - 1; i >= 0; i--, v >>= 8) dm8w(a + i, (uint8_t)v);
}

void Rsp_StoreDmem32(Rsp *rsp, uint32_t a, uint32_t v) { store(rsp, a, 4, v); }
uint32_t Rsp_LoadDmem32(const Rsp *rsp, uint32_t a) { return load((Rsp *)rsp, a, 4, 0); }

static void step(Rsp *rsp) {
    const uint8_t *imem = rsp->imem;
    uint32_t *r = rsp->r;
    uint32_t pc = rsp->pc;
    uint32_t op = (uint32_t)imem[pc] << 24 | imem[pc + 1] << 16 | imem[pc + 2] << 8 | imem[pc + 3];
    uint32_t cur = pc;
    rsp->nnpc = (rsp->npc + 4) & 0xFFC;
    uint32_t rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, sa = (op >> 6) & 31;
    int32_t simm = (int16_t)op;
    uint32_t uimm = op & 0xFFFF;
    uint32_t res = 0;
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
        case 0x08: rsp->nnpc = r[rs] & 0xFFC; break;
        case 0x09: res = (cur + 8) & 0xFFC; wr = rd; rsp->nnpc = r[rs] & 0xFFC; break;
        case 0x0D: rsp->halted = 1; break; /* BREAK */
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
        case 0x00: branch(rsp, (int32_t)r[rs] < 0, simm); break;
        case 0x01: branch(rsp, (int32_t)r[rs] >= 0, simm); break;
        case 0x10: { int t = (int32_t)r[rs] < 0; r[31] = (cur + 8) & 0xFFC; branch(rsp, t, simm); break; }
        case 0x11: { int t = (int32_t)r[rs] >= 0; r[31] = (cur + 8) & 0xFFC; branch(rsp, t, simm); break; }
        default: goto bad;
        }
        break;
    case 0x02: rsp->nnpc = (op << 2) & 0xFFC; break;
    case 0x03: r[31] = (cur + 8) & 0xFFC; rsp->nnpc = (op << 2) & 0xFFC; break;
    case 0x04: branch(rsp, r[rs] == r[rt], simm); break;
    case 0x05: branch(rsp, r[rs] != r[rt], simm); break;
    case 0x06: branch(rsp, (int32_t)r[rs] <= 0, simm); break;
    case 0x07: branch(rsp, (int32_t)r[rs] > 0, simm); break;
    case 0x08: case 0x09: res = r[rs] + (uint32_t)simm; wr = rt; break;
    case 0x0A: res = (int32_t)r[rs] < simm; wr = rt; break;
    case 0x0B: res = r[rs] < (uint32_t)simm; wr = rt; break;
    case 0x0C: res = r[rs] & uimm; wr = rt; break;
    case 0x0D: res = r[rs] | uimm; wr = rt; break;
    case 0x0E: res = r[rs] ^ uimm; wr = rt; break;
    case 0x0F: res = uimm << 16; wr = rt; break;
    case 0x10:
        if (rs == 0) { res = mfc0(rsp, rd); wr = rt; }
        else if (rs == 4) mtc0(rsp, rd, r[rt]);
        else goto bad;
        break;
    case 0x12:
        if (op & (1u << 25)) { vector_op(rsp, op); break; }
        switch (rs) {
        case 0x00: { int e = (op >> 7) & 15; res = (uint32_t)(int16_t)(vbyte(&rsp->vr[rd], e) << 8 | vbyte(&rsp->vr[rd], e + 1)); wr = rt; break; }
        case 0x02:
            res = rd == 0 ? rsp->vco : rd == 1 ? rsp->vcc : rsp->vce;
            res = rd == 2 ? res : (uint32_t)(int16_t)res;
            wr = rt;
            break;
        case 0x04: {
            int e = (op >> 7) & 15;
            set_vbyte(&rsp->vr[rd], e, (uint8_t)(r[rt] >> 8));
            if (e + 1 < 16) set_vbyte(&rsp->vr[rd], e + 1, (uint8_t)r[rt]);
            break;
        }
        case 0x06:
            if (rd == 0) rsp->vco = (uint16_t)r[rt];
            else if (rd == 1) rsp->vcc = (uint16_t)r[rt];
            else rsp->vce = (uint8_t)r[rt];
            break;
        default: goto bad;
        }
        break;
    case 0x20: res = load(rsp, r[rs] + simm, 1, 1); wr = rt; break;
    case 0x21: res = load(rsp, r[rs] + simm, 2, 1); wr = rt; break;
    case 0x23: res = load(rsp, r[rs] + simm, 4, 0); wr = rt; break;
    case 0x24: res = load(rsp, r[rs] + simm, 1, 0); wr = rt; break;
    case 0x25: res = load(rsp, r[rs] + simm, 2, 0); wr = rt; break;
    case 0x28: store(rsp, r[rs] + simm, 1, r[rt]); break;
    case 0x29: store(rsp, r[rs] + simm, 2, r[rt]); break;
    case 0x2B: store(rsp, r[rs] + simm, 4, r[rt]); break;
    case 0x32: lwc2(rsp, op); break;
    case 0x3A: swc2(rsp, op); break;
    default: goto bad;
    }
    if (wr > 0) r[wr] = res;
    rsp->pc = rsp->npc;
    rsp->npc = rsp->nnpc;
    return;
bad: {
    char msg[64];
    snprintf(msg, sizeof msg, "RSP: instruction %08x at %03x not implemented", op, cur);
    fatal(rsp, msg);
}
}

void Rsp_Reset(Rsp *rsp, uint32_t pc) {
    memset(rsp->r, 0, sizeof rsp->r);
    rsp->halted = 0;
    rsp->pc = pc & 0xFFC;
    rsp->npc = (pc + 4) & 0xFFC;
}

int Rsp_Run(Rsp *rsp, uint64_t max_steps) {
    for (uint64_t n = 0; !rsp->halted; n++) {
        if (n > max_steps) {
            for (int i = 0; i < 32; i++) {
                uint32_t h = rsp->pc_hist[(n + i) & 31];
                fprintf(stderr, "  pc %03x op %02x%02x%02x%02x\n", h, rsp->imem[h], rsp->imem[h + 1],
                        rsp->imem[h + 2], rsp->imem[h + 3]);
            }
            return 0;
        }
        rsp->pc_hist[n & 31] = rsp->pc;
        step(rsp);
    }
    return 1;
}
