/* The game's sound, native: libultra's audio microcode (aspMain, ABI 1, the one the game's
 * synthesizer writes its command lists for) as C, running each audio task's command list on the CPU.
 * No microcode runs in play (docs/AUDIO.md).
 *
 * The command list is built by native code (libultra's synthesizer, compiled for the host), so its
 * words are host-order and its addresses are host addresses below 4 GiB, or KSEG0/physical RDRAM
 * addresses (port/os K0_TO_PHYS). All the buffers it reads and writes (ADPCM and resampler states,
 * envelope states, sample and output buffers) are native too, so DMEM is kept in host order.
 *
 * Every command gives the microcode's results bit for bit, checked against the microcode itself
 * (port/audio/rsp_lle.c, tests only: ELTALE_AUDIO_CHECK and ELTALE_AUDIO_FUZZ, tests/audio_checks.sh):
 * - its rounding (each vmulf rounds), its 8- and 16-sample blocks, its 48-bit accumulator and
 *   clamps, its state layouts;
 * - its scratch area at DMEM 0xF90 (here dmem[SCRATCH]), which states pass through and which POLEF's
 *   INIT reads stale;
 * - its parameter block (SETBUFF, SETVOL and SETLOOP share it) and its ADPCM book, which every task
 *   starts from the microcode's data, as the RSP's boot loads it over DMEM 0-0x7FF;
 * - its DMA (addresses down to 8 bytes, lengths up to 8), and the order of its loads and stores where
 *   a command's output may run into its input (RESAMPLE, MIXER, INTERLEAVE, POLEF).
 * Buffer offsets are the command lists' own; the microcode adds 0x5C0. The resampler's filter table
 * and the data the RSP's boot loads are the microcode's own, read from the task's ucode data in RDRAM
 * (from the user's ROM). Buffers are taken to be 16-byte aligned, as libultra keeps them (the vector
 * unit's loads and stores would split unaligned ones). */
#include "port/audio.h"

#include <stdint.h>
#include <string.h>

__attribute__((noreturn)) void Port_HostFatal(const char *what);

/* command flags: A_INIT, A_LOOP, A_LEFT, A_VOL, A_AUX (PR/abi.h, via PR/libaudio.h) */

static u8 dmem[0x1000];
static s16 adpcm_table[0x80];
static s16 resample_lut[64 * 4];
static int lut_ready;

/* The commands' parameters, kept as the microcode keeps them (DMEM 0x360-0x37F, halfwords): SETBUFF
 * stores buffer offsets with 0x5C0 added, and SETLOOP's 32-bit address lies over the left target and
 * rate hi. Every task starts them from the microcode's data, as the RSP's boot loads it. */
enum { P_IN, P_OUT, P_COUNT, P_VOL_L, P_VOL_R, P_DRY_R, P_WET_L, P_WET_R,
       P_TARGET_L, P_RATE_L_HI, P_RATE_L_LO, P_TARGET_R, P_RATE_R_HI, P_RATE_R_LO, P_DRY, P_WET };
static u16 par[16];
static u32 loop_host; /* SETLOOP's address as the list gave it (a host address) */
#define BUF(i) ((u16)(par[i] - 0x5C0)) /* a buffer offset, the command list's own */

#define SCRATCH (0xF90 - 0x5C0)

/* tests only (the reference check): told before each DRAM write */
void (*Port_AspWriteHook)(void *dst, u32 bytes);

/* A DRAM address the command is about to write `bytes` at. */
static void *dram(u32 a);
static void *dram_w(u32 a, u32 bytes) {
    void *p = dram(a);
    if (Port_AspWriteHook) Port_AspWriteHook(p, bytes);
    return p;
}

static s16 *s16_at(u32 off) { return (s16 *)(dmem + (off & 0xFFE)); }

/* the RSP's accumulator results: 48 bits, read back as a clamped middle or low slice */
static int64_t sext48(int64_t a) { return (int64_t)((uint64_t)a << 16) >> 16; }
static s16 sat_mid(int64_t acc) { int64_t v = sext48(acc) >> 16; return v < -32768 ? -32768 : v > 32767 ? 32767 : (s16)v; }
static u16 sat_lo(int64_t acc) {
    int64_t v = sext48(acc) >> 16;
    return v < -32768 ? 0 : v > 32767 ? 0xFFFF : (u16)acc;
}
static s16 vmulf(s16 a, s16 b) { return sat_mid((int64_t)a * b * 2 + 0x8000); }
static s16 vadd(s16 a, s16 b) { s32 v = a + b; return v < -32768 ? -32768 : v > 32767 ? 32767 : (s16)v; }
/* out = out * 0x7FFF + in * gain, as vmulf then vmacf */
static s16 mix(s16 out, s16 in, s16 gain) { return sat_mid((int64_t)out * 0x7FFF * 2 + 0x8000 + (int64_t)in * gain * 2); }
static s16 clamp16(s32 v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (s16)v; }
static u32 align(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }

/* A DRAM address in a command: a host address, or KSEG0 / physical RDRAM. */
static void *dram(u32 a) {
    if (a >= 0x80000000u && a < 0x80800000u) return (void *)(uptr)a;
    if (a < 0x00800000u) return (void *)(uptr)(a | 0x80000000u);
    if (a >= 0x10000000u) return (void *)(uptr)a;
    Port_HostFatal("aspMain: unexpected DRAM address");
}

static void load_lut(u32 ucode_data) {
    /* the 64x4 filter table sits 0xC0 bytes into aspMain's data; it is symmetric (last row is the
     * first reversed), which the check below uses to make sure we found it */
    const u8 *p = (const u8 *)(uptr)(ucode_data | 0x80000000u) + 0xC0;
    for (int i = 0; i < 256; i++) resample_lut[i] = (s16)(p[i * 2] << 8 | p[i * 2 + 1]);
    for (int i = 0; i < 4; i++)
        if (resample_lut[i] != resample_lut[255 - i] || !resample_lut[1])
            Port_HostFatal("aspMain: resample table not found in the microcode data");
    lut_ready = 1;
}

static void residuals(s16 *dst, const s16 *src, const s16 *cb, const s16 *last, unsigned n) {
    const s16 *book1 = cb, *book2 = cb + 8;
    s16 l1 = last[0], l2 = last[1];
    for (unsigned i = 0; i < n; i++) {
        s32 accu = (s32)src[i] << 11;
        accu += book1[i] * l1 + book2[i] * l2;
        for (unsigned k = 0; k < i; k++) accu += book2[i - 1 - k] * src[k];
        dst[i] = clamp16(accu >> 11);
    }
}

/* ADPCM's A_LOOP state address: what SETLOOP left, unless a SETVOL wrote over it since (then what
 * the microcode would read there, as an address in the image's first 16 MiB) */
static u32 loop_addr(void) {
    u32 a = (u32)par[P_TARGET_L] << 16 | par[P_RATE_L_HI];
    return a == (loop_host & 0x00FFFFFFu) ? loop_host : 0x10000000u | (a & 0x00FFFFFFu);
}

static void adpcm(int init, int loop, u16 out, u16 in, u32 count, u32 state) {
    s16 last[16];
    if (init) memset(last, 0, sizeof last);
    else memcpy(last, loop ? (s16 *)dram(loop_addr()) : (s16 *)dram(state), sizeof last);
    memcpy(s16_at(out), last, 32);
    out += 32;
    for (; count; count -= 32, out += 32) {
        s16 frame[16];
        u8 code = dmem[in++ & 0xFFF];
        u8 scale = code >> 4;
        /* the book is DMEM 0x4C0 + predictor * 32: predictors 8-15 (none in the game's data) are
         * the buffers' first 0x100 bytes */
        const s16 *cb = (code & 0x8) ? s16_at(((code & 0x7) << 5)) : adpcm_table + ((code & 0x7) << 4);
        unsigned rshift = scale < 12 ? 12 - scale : 0;
        for (int i = 0; i < 8; i++) {
            u8 b = dmem[in++ & 0xFFF];
            frame[i * 2] = (s16)((u16)(b & 0xF0) << 8) >> rshift;
            frame[i * 2 + 1] = (s16)((u16)(b & 0x0F) << 12) >> rshift;
        }
        residuals(last, frame, cb, last + 14, 8);
        residuals(last + 8, frame + 8, cb, last + 6, 8);
        memcpy(s16_at(out), last, 32);
    }
    memcpy(dram_w(state, sizeof last), last, sizeof last);
}

/* The state (0x20 bytes, through the scratch): the 4 samples before the next position, its
 * fraction, then (for A_LOOP, unused by libultra) an alignment and 8 samples. Each output is
 * sat(sat(f0 + f1) + sat(f2 + f3)), each f a rounded vmulf; 8 outputs per block. */
static void resample(int init, u16 out, u16 in, u32 count, u32 pitch, u32 state) {
    s16 *sc = s16_at(SCRATCH), *d = (s16 *)dmem;
    if (init) { memset(sc, 0, 8); sc[4] = 0; }
    else memcpy(sc, dram(state), 0x20);
    u32 base = (in >> 1) - 4; /* the 4 state samples go just before the input */
    for (int k = 0; k < 4; k++) d[(base + k) & 0x7FF] = sc[k];
    u32 pos = (u16)sc[4], step = pitch;
    u32 blocks = count ? (count + 15) / 16 : 1, n;
    for (n = 0; n < blocks * 8; n += 8) {
        s16 block[8]; /* a block is read, then stored: the output may run into the input */
        for (int i = 0; i < 8; i++, pos += step) {
            u32 ip = base + (pos >> 16);
            const s16 *lut = resample_lut + ((pos >> 10) & 0x3F) * 4;
            s16 f0 = vmulf(d[ip & 0x7FF], lut[0]), f1 = vmulf(d[(ip + 1) & 0x7FF], lut[1]);
            s16 f2 = vmulf(d[(ip + 2) & 0x7FF], lut[2]), f3 = vmulf(d[(ip + 3) & 0x7FF], lut[3]);
            block[i] = vadd(vadd(f0, f1), vadd(f2, f3));
        }
        for (int i = 0; i < 8; i++) d[((out >> 1) + n + i) & 0x7FF] = block[i];
    }
    /* what the last block leaves in the scratch: the next block's sample and table addresses */
    for (int i = 0; i < 8; i++) {
        u32 p = pos + i * step;
        sc[0x10 + i] = (s16)(0x5C0 + in - 8 + 2 * (p >> 16));
        sc[0x18 + i] = (s16)(0xC0 + ((p >> 10) & 0x3F) * 8);
    }
    u32 ip = base + (pos >> 16);
    for (int k = 0; k < 4; k++) sc[k] = d[(ip + k) & 0x7FF];
    sc[4] = (s16)pos;
    u32 at = (ip + 4) * 2, mis = (at - in) & 0xF;
    at -= mis;
    sc[5] = (s16)(mis ? 16 - mis : 0);
    memcpy(sc + 8, dmem + (at & 0xFFE), 16);
    memcpy(dram_w(state, 0x20), sc, 0x20);
}

/* The envelope mixer. Each side's volume is 8 lanes of 16.16 (one per sample of a block): INIT
 * spreads them from the volume to volume * rate across the first block; each later block
 * multiplies every lane by the rate. The integer part is held at the target (unsigned min when
 * rising, signed max when falling). The state (0x50 bytes, through the scratch) is the left lanes
 * (integer, fraction), the right lanes, then target, rate and gain parameters. */
typedef struct { s16 hi[8]; u16 lo[8]; } Ramp;

static void ramp_init(Ramp *r, s16 vol, s16 rate_hi, u16 rate_lo) {
    static const u16 frac[8] = { 0x2000, 0x4000, 0x6000, 0x8000, 0xA000, 0xC000, 0xE000, 0xFFFF };
    int64_t acc = (int64_t)vol * rate_lo + (int64_t)vol * rate_hi * 65536;
    s32 diff = sat_mid(acc) - vol; /* vsub */
    s16 d_hi = diff < -32768 ? -32768 : diff > 32767 ? 32767 : (s16)diff;
    u16 d_lo = sat_lo(acc);
    for (int i = 0; i < 8; i++) {
        int64_t a = (((uint32_t)frac[i] * d_lo) >> 16) + (int64_t)frac[i] * d_hi + (int64_t)vol * 65536;
        r->hi[i] = sat_mid(a);
        r->lo[i] = sat_lo(a);
    }
}

static void ramp_next(Ramp *r, s16 rate_hi, u16 rate_lo) {
    for (int i = 0; i < 8; i++) {
        int64_t a = (((uint32_t)r->lo[i] * rate_lo) >> 16) + (int64_t)r->hi[i] * rate_lo +
                    (int64_t)r->lo[i] * rate_hi + (int64_t)r->hi[i] * rate_hi * 65536;
        r->hi[i] = sat_mid(a);
        r->lo[i] = sat_lo(a);
    }
}

static void ramp_clamp(Ramp *r, s16 rate_hi, s16 target) {
    for (int i = 0; i < 8; i++) {
        if (rate_hi > 0) { if ((u16)r->hi[i] >= (u16)target) r->hi[i] = target; } /* vcl */
        else if (r->hi[i] < target) r->hi[i] = target;                              /* vge */
    }
}

static void env_block(const Ramp *r, s16 dry, s16 wet, const s16 *in, s16 *dry_out, s16 *wet_out) {
    for (int i = 0; i < 8; i++) {
        s16 gd = vmulf(r->hi[i], dry), gw = vmulf(r->hi[i], wet);
        dry_out[i] = mix(dry_out[i], in[i], gd);
        if (wet_out) wet_out[i] = mix(wet_out[i], in[i], gw);
    }
}

static void envmixer(int init, int aux, u32 state) {
    s16 *sc = s16_at(SCRATCH);
    Ramp left, right;
    s16 p[8]; /* target L, rate L hi, rate L lo, target R, rate R hi, rate R lo, dry, wet */
    if (init) memcpy(p, par + P_TARGET_L, sizeof p);
    else {
        memcpy(sc, dram(state), 0x50);
        memcpy(&left, sc, 32);
        memcpy(&right, sc + 16, 32);
        memcpy(p, sc + 32, 16);
    }
    const s16 *in = s16_at(BUF(P_IN));
    s16 *dl = s16_at(BUF(P_OUT)), *dr = s16_at(BUF(P_DRY_R));
    s16 *wl = aux ? s16_at(BUF(P_WET_L)) : NULL, *wr = aux ? s16_at(BUF(P_WET_R)) : NULL;
    /* the microcode counts down a signed count, 16 bytes a block, after the first block (INIT:
     * after the second) */
    s16 count = (s16)par[P_COUNT];
    u32 blocks = count > 0 ? ((u32)count + 15) / 16 : 1;
    if (init && blocks < 2) blocks = 2;
    for (u32 b = 0; b < blocks; b++) {
        if (b == 0 && init) {
            ramp_init(&left, (s16)par[P_VOL_L], p[1], (u16)p[2]);
            ramp_init(&right, (s16)par[P_VOL_R], p[4], (u16)p[5]);
        } else {
            ramp_next(&left, p[1], (u16)p[2]);
            ramp_next(&right, p[4], (u16)p[5]);
        }
        ramp_clamp(&left, p[1], p[0]);
        ramp_clamp(&right, p[4], p[3]);
        env_block(&left, p[6], p[7], in + b * 8, dl + b * 8, wl ? wl + b * 8 : NULL);
        env_block(&right, p[6], p[7], in + b * 8, dr + b * 8, wr ? wr + b * 8 : NULL);
    }
    memcpy(sc, &left, 32);
    memcpy(sc + 16, &right, 32);
    memcpy(sc + 32, p, 16);
    memcpy(dram_w(state, 0x50), sc, 0x50);
}

/* The pole filter. Its coefficient table's second row is scaled by gain << 2 in place; INIT
 * clears only the first half of the scratch it reads its history from, so it starts from whatever
 * history the scratch held. The state is the last 4 outputs. */
static void polef(int init, u16 out, u16 in, u32 count, s16 gain, u32 state) {
    if (!count) return;
    s16 *sc = s16_at(SCRATCH);
    sc[0] = sc[1] = 0;
    if (!init) memcpy(sc, dram(state), 8);
    s16 y2 = sc[2], y1 = sc[3];
    const s16 *r0 = adpcm_table, *r1 = adpcm_table + 8;
    s16 h[8];
    for (int i = 0; i < 8; i++) h[i] = (s16)(((s32)r1[i] * (u16)(gain << 2)) >> 16);
    s16 *dst = s16_at(out);
    for (u32 blk = 0; blk < (count + 15) / 16; blk++, dst += 8, in += 16) {
        s16 x[8]; /* a block at a time: in and out may be the same buffer */
        memcpy(x, s16_at(in), sizeof x);
        for (int i = 0; i < 8; i++) {
            s32 sum = (s32)((u32)(r0[i] * y2) + (u32)(r1[i] * y1) + (u32)(x[i] * gain));
            for (int k = 0; k < i; k++) sum = (s32)((u32)sum + (u32)(h[i - 1 - k] * x[k]));
            dst[i] = sat_mid((int64_t)sum * 4);
        }
        y2 = dst[6];
        y1 = dst[7];
    }
    memcpy(adpcm_table + 8, h, sizeof h);
    memcpy(dram_w(state, 8), dst - 4, 8);
}

/* The RSP's DMA as the microcode starts it (length - 1 in the length register): both addresses
 * rounded down to 8 bytes, the length up to 8, and a length over 4 KiB taken as rows of what is
 * left over (the register's count field). DMEM wraps at its end. */
static u32 dma_len(u32 count) { return count ? (((count - 1) & 0xFFF) | 7) + 1 : 0; }
static void dma(int to_dram, u32 off, u32 addr, u32 count) {
    if (!count) return;
    u32 len = dma_len(count), rows = ((count - 1) >> 12) + 1, total = len * rows;
    u32 mem = ((off + 0x5C0) & 0xFF8) - 0x5C0; /* the command list's offsets, as the microcode's */
    u8 *p = to_dram ? dram_w(addr & ~7u, total) : dram(addr & ~7u);
    mem &= 0xFFF;
    if (mem + total <= sizeof dmem) { /* no wrap: the usual case */
        if (to_dram) memcpy(p, dmem + mem, total);
        else memcpy(dmem + mem, p, total);
        return;
    }
    for (u32 i = 0; i < total; i++, p++) {
        u8 *d = dmem + ((mem + i) & 0xFFF);
        if (to_dram) *p = *d;
        else *d = *p;
    }
}

/* MIXER: out = out * 0x7FFF + in * gain, 16 samples a step, with the microcode's order of loads and
 * stores (the next step's first input half is read after this step's first output half is stored) */
static void mixer(s16 gain, u16 out, u16 in, u16 count) {
    s16 d0[8], d1[8], s0[8], s1[8];
    for (int i = 0; i < 8; i++) {
        d0[i] = *s16_at(out + i * 2); s0[i] = *s16_at(in + i * 2);
        d1[i] = *s16_at(out + 16 + i * 2); s1[i] = *s16_at(in + 16 + i * 2);
    }
    for (s32 n = count; n > 0; n -= 32) {
        in += 32;
        for (int i = 0; i < 8; i++) *s16_at(out + i * 2) = mix(d0[i], s0[i], gain);
        for (int i = 0; i < 8; i++) s0[i] = *s16_at(in + i * 2);
        s16 o1[8];
        for (int i = 0; i < 8; i++) o1[i] = mix(d1[i], s1[i], gain);
        for (int i = 0; i < 8; i++) s1[i] = *s16_at(in + 16 + i * 2);
        for (int i = 0; i < 8; i++) *s16_at(out + 16 + i * 2) = o1[i];
        out += 32;
        for (int i = 0; i < 8; i++) { d0[i] = *s16_at(out + i * 2); d1[i] = *s16_at(out + 16 + i * 2); }
    }
}

/* One command (Port_RunAudioTask below; port/audio/rsp_lle.c checks them one at a time). */
void Port_AspCommand(const OSTask *task, u32 w0, u32 w1) {
    if (!lut_ready) load_lut((u32)(uptr)task->t.ucode_data);
    {
        u8 op = w0 >> 24, flags = (w0 >> 16) & 0xFF;
        switch (op) {
        case 0: break; /* SPNOOP */
        case 1: adpcm(flags & A_INIT, flags & A_LOOP, BUF(P_OUT), BUF(P_IN), align(par[P_COUNT], 32), w1); break;
        case 2: /* CLEARBUFF, 16 bytes at a time */
            for (u32 n = 0, at = w0 & 0xFFFF, len = align(w1 & 0xFFFF, 16); n < len; n++) dmem[(at + n) & 0xFFF] = 0;
            break;
        case 3: envmixer(flags & A_INIT, flags & A_AUX, w1); break;
        case 4: dma(0, BUF(P_IN), w1, par[P_COUNT]); break; /* LOADBUFF */
        case 5: resample(flags & A_INIT, BUF(P_OUT), BUF(P_IN), par[P_COUNT], (w0 & 0xFFFF) << 1, w1); break;
        case 6: dma(1, BUF(P_OUT), w1, par[P_COUNT]); break; /* SAVEBUFF */
        case 7: if (w1 & 0x00FFFFFF) Port_HostFatal("aspMain: segments are not used by this game"); break;
        case 8: /* SETBUFF */
            if (flags & A_AUX) { par[P_DRY_R] = (u16)(w0 + 0x5C0); par[P_WET_L] = (u16)((w1 >> 16) + 0x5C0); par[P_WET_R] = (u16)(w1 + 0x5C0); }
            else { par[P_IN] = (u16)(w0 + 0x5C0); par[P_OUT] = (u16)((w1 >> 16) + 0x5C0); par[P_COUNT] = (u16)w1; }
            break;
        case 9: /* SETVOL */
            if (flags & A_AUX) { par[P_DRY] = (u16)w0; par[P_WET] = (u16)w1; }
            else if (flags & A_VOL) par[(flags & A_LEFT) ? P_VOL_L : P_VOL_R] = (u16)w0;
            else {
                int at = (flags & A_LEFT) ? P_TARGET_L : P_TARGET_R;
                par[at] = (u16)w0;
                par[at + 1] = (u16)(w1 >> 16);
                par[at + 2] = (u16)w1;
            }
            break;
        case 10: /* DMEMMOVE, 16 bytes at a time */
            for (u32 n = 0; n < (w1 & 0xFFFF); n += 16) {
                u8 tmp[16];
                memcpy(tmp, dmem + (((w0 & 0xFFFF) + n) & 0xFFF), 16);
                memcpy(dmem + ((((w1 >> 16) & 0xFFFF) + n) & 0xFFF), tmp, 16);
            }
            break;
        case 11: { /* LOADADPCM: to DMEM 0x4C0, the book; a longer one runs on into the buffers */
            u32 len = dma_len(w0 & 0xFFFF);
            const u8 *src = dram(w1 & ~7u);
            memcpy(adpcm_table, src, len < sizeof adpcm_table ? len : sizeof adpcm_table);
            if (len > sizeof adpcm_table) memcpy(dmem, src + sizeof adpcm_table, len - sizeof adpcm_table);
            break; }
        case 12: mixer((s16)w0, (u16)w1, (u16)(w1 >> 16), par[P_COUNT]); break;
        case 13: { /* INTERLEAVE: 8 samples of each side read, then 8 pairs stored, per 16 bytes of count */
            u32 out = BUF(P_OUT), l = (u16)(w1 >> 16), r = (u16)w1;
            for (s32 n = par[P_COUNT]; n > 0; n -= 16, l += 16, r += 16, out += 32) {
                s16 a[8], b[8];
                for (int i = 0; i < 8; i++) { a[i] = *s16_at(l + i * 2); b[i] = *s16_at(r + i * 2); }
                for (int i = 0; i < 8; i++) { *s16_at(out + i * 4) = a[i]; *s16_at(out + i * 4 + 2) = b[i]; }
            }
            break; }
        case 14: polef(flags & A_INIT, BUF(P_OUT), BUF(P_IN), par[P_COUNT], (s16)w0, w1); break;
        case 15: /* SETLOOP: the microcode keeps the address's low 24 bits (plus segment 0's base: 0) */
            loop_host = w1;
            par[P_TARGET_L] = (u16)((w1 & 0x00FFFFFFu) >> 16);
            par[P_RATE_L_HI] = (u16)w1;
            break;
        default: Port_HostFatal("aspMain: unknown command");
        }
    }
}

u8 *Port_AspDmem(void) { return dmem; }
/* tests only: everything a command list leaves for the next (DMEM, the ADPCM book, the SETBUFF,
 * SETVOL and SETLOOP values), so the reference check can run a list again from the same start */
static struct { u8 dmem[sizeof dmem]; s16 adpcm_table[0x80]; u16 par[16]; u32 loop_host; } saved;
void Port_AspSaveState(void) {
    memcpy(saved.dmem, dmem, sizeof dmem);
    memcpy(saved.adpcm_table, adpcm_table, sizeof adpcm_table);
    memcpy(saved.par, par, sizeof par);
    saved.loop_host = loop_host;
}
void Port_AspRestoreState(void) {
    memcpy(dmem, saved.dmem, sizeof dmem);
    memcpy(adpcm_table, saved.adpcm_table, sizeof adpcm_table);
    memcpy(par, saved.par, sizeof par);
    loop_host = saved.loop_host;
}

/* What a task starts from. The RSP's boot loads the microcode's data (ucode_data_size bytes, 0x800
 * for this game) at DMEM 0: the parameters (0x360), the ADPCM book (0x4C0, which POLEF scales in
 * place) and the first bytes of the buffers (from 0x5C0; a list that saves part of a buffer it has
 * not filled saves those bytes) start every task as that data. The rest of DMEM is as the last task
 * left it. */
static u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
void Port_AspTaskStart(const OSTask *task) {
    u32 size = task->t.ucode_data_size;
    const u8 *data = (const u8 *)(uptr)((u32)(uptr)task->t.ucode_data | 0x80000000u);
    if (size > 0x1000) size = 0x1000;
    if (size < 0x5C0) Port_HostFatal("aspMain: the task's microcode data is too short");
    for (int i = 0; i < 16; i++) par[i] = be16(data + 0x360 + i * 2);
    for (int i = 0; i < 0x80; i++) adpcm_table[i] = (s16)be16(data + 0x4C0 + i * 2);
    for (u32 i = 0; i < size - 0x5C0; i++) dmem[i ^ 1] = data[0x5C0 + i]; /* big-endian halfwords */
}

void Port_RunAudioTask(const OSTask *task) {
    const u32 *cmd = (const u32 *)task->t.data_ptr;
    u32 words = task->t.data_size / 4;
    Port_AspTaskStart(task);
    for (u32 i = 0; i + 1 < words; i += 2) Port_AspCommand(task, cmd[i], cmd[i + 1]);
}
