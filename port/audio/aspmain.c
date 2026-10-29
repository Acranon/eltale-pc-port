/* aspMain (libultra's audio microcode, ABI 1) as C: runs an audio task's command list on the CPU.
 *
 * The command list is built by native code (libultra's synthesizer, compiled for the host), so its
 * words are host-order and its addresses are host addresses below 4 GiB, or KSEG0/physical RDRAM
 * addresses (port/os K0_TO_PHYS). All the buffers it reads and writes (ADPCM and resampler states,
 * envelope states, sample and output buffers) are native too, so DMEM is kept in host order.
 *
 * Every command gives the microcode's results bit for bit, checked against the microcode itself
 * (port/audio/rsp_lle.c, ELTALE_AUDIO_CHECK=1): its rounding (each vmulf rounds), its 8- and
 * 16-sample blocks, its state layouts, and its scratch area at DMEM 0xF90 (here dmem[SCRATCH]),
 * which states pass through and which POLEF's INIT reads stale. Buffer offsets are the command
 * lists' own; the microcode adds 0x5C0. The resampler's filter table is the microcode's own, read
 * from the task's ucode data in RDRAM (from the user's ROM). */
#include "port/audio.h"

#include <stdint.h>
#include <string.h>

__attribute__((noreturn)) void Port_HostFatal(const char *what);

/* command flags: A_INIT, A_LOOP, A_LEFT, A_VOL, A_AUX (PR/abi.h, via PR/libaudio.h) */

static u8 dmem[0x1000];
static s16 adpcm_table[0x80];
static s16 resample_lut[64 * 4];
static int lut_ready;

static struct {
    u16 in, out, count;
    u16 dry_right, wet_left, wet_right;
    s16 dry, wet, vol[2], target[2];
    s32 rate[2];
    u32 loop;
} st;

#define SCRATCH (0xF90 - 0x5C0)

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

static void adpcm(int init, int loop, u16 out, u16 in, u32 count, u32 state) {
    s16 last[16];
    s16 *state_p = dram(state);
    if (init) memset(last, 0, sizeof last);
    else memcpy(last, loop ? (s16 *)dram(st.loop) : state_p, sizeof last);
    memcpy(s16_at(out), last, 32);
    out += 32;
    for (; count; count -= 32, out += 32) {
        s16 frame[16];
        u8 code = dmem[in++ & 0xFFF];
        u8 scale = code >> 4;
        const s16 *cb = adpcm_table + ((code & 0xF) << 4);
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
    memcpy(state_p, last, sizeof last);
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
    for (n = 0; n < blocks * 8; n++, pos += step) {
        u32 ip = base + (pos >> 16);
        const s16 *lut = resample_lut + ((pos >> 10) & 0x3F) * 4;
        s16 f0 = vmulf(d[ip & 0x7FF], lut[0]), f1 = vmulf(d[(ip + 1) & 0x7FF], lut[1]);
        s16 f2 = vmulf(d[(ip + 2) & 0x7FF], lut[2]), f3 = vmulf(d[(ip + 3) & 0x7FF], lut[3]);
        d[((out >> 1) + n) & 0x7FF] = vadd(vadd(f0, f1), vadd(f2, f3));
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
    memcpy(dram(state), sc, 0x20);
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
    if (init) {
        p[0] = st.target[0]; p[1] = (s16)(st.rate[0] >> 16); p[2] = (s16)st.rate[0];
        p[3] = st.target[1]; p[4] = (s16)(st.rate[1] >> 16); p[5] = (s16)st.rate[1];
        p[6] = st.dry; p[7] = st.wet;
    } else {
        memcpy(sc, dram(state), 0x50);
        memcpy(&left, sc, 32);
        memcpy(&right, sc + 16, 32);
        memcpy(p, sc + 32, 16);
    }
    const s16 *in = s16_at(st.in);
    s16 *dl = s16_at(st.out), *dr = s16_at(st.dry_right);
    s16 *wl = aux ? s16_at(st.wet_left) : NULL, *wr = aux ? s16_at(st.wet_right) : NULL;
    u32 blocks = (st.count + 15) / 16;
    if (blocks < 1) blocks = 1;
    if (init && blocks < 2) blocks = 2;
    for (u32 b = 0; b < blocks; b++) {
        if (b == 0 && init) {
            ramp_init(&left, st.vol[0], p[1], (u16)p[2]);
            ramp_init(&right, st.vol[1], p[4], (u16)p[5]);
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
    memcpy(dram(state), sc, 0x50);
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
    memcpy(dram(state), dst - 4, 8);
}

/* One command (Port_RunAudioTask below; port/audio/rsp_lle.c checks them one at a time). */
void Port_AspCommand(const OSTask *task, u32 w0, u32 w1) {
    if (!lut_ready) load_lut((u32)(uptr)task->t.ucode_data);
    {
        u8 op = w0 >> 24, flags = (w0 >> 16) & 0xFF;
        switch (op) {
        case 0: break; /* SPNOOP */
        case 1: adpcm(flags & A_INIT, flags & A_LOOP, st.out, st.in, align(st.count, 32), w1); break;
        case 2: memset(dmem + (w0 & 0xFFF), 0, align(w1 & 0xFFFF, 16) & 0xFFF); break; /* CLEARBUFF */
        case 3: envmixer(flags & A_INIT, flags & A_AUX, w1); break;
        case 4: if (st.count) memcpy(dmem + (st.in & 0xFFF), dram(w1 & ~3u), st.count); break; /* LOADBUFF */
        case 5: resample(flags & A_INIT, st.out, st.in, st.count, (w0 & 0xFFFF) << 1, w1); break;
        case 6: if (st.count) memcpy(dram(w1 & ~3u), dmem + (st.out & 0xFFF), st.count); break; /* SAVEBUFF */
        case 7: if (w1 & 0x00FFFFFF) Port_HostFatal("aspMain: segments are not used by this game"); break;
        case 8: /* SETBUFF */
            if (flags & A_AUX) { st.dry_right = w0; st.wet_left = w1 >> 16; st.wet_right = w1; }
            else { st.in = w0; st.out = w1 >> 16; st.count = w1; }
            break;
        case 9: /* SETVOL */
            if (flags & A_AUX) { st.dry = (s16)w0; st.wet = (s16)w1; }
            else {
                int lr = (flags & A_LEFT) ? 0 : 1;
                if (flags & A_VOL) st.vol[lr] = (s16)w0;
                else { st.target[lr] = (s16)w0; st.rate[lr] = (s32)w1; }
            }
            break;
        case 10: /* DMEMMOVE, 16 bytes at a time */
            for (u32 n = 0; n < (w1 & 0xFFFF); n += 16) {
                u8 tmp[16];
                memcpy(tmp, dmem + (((w0 & 0xFFFF) + n) & 0xFFF), 16);
                memcpy(dmem + ((((w1 >> 16) & 0xFFFF) + n) & 0xFFF), tmp, 16);
            }
            break;
        case 11: memcpy(adpcm_table, dram(w1), (w0 & 0xFFFF) > sizeof adpcm_table ? sizeof adpcm_table : (w0 & 0xFFFF)); break;
        case 12: { /* MIXER */
            s16 gain = (s16)w0;
            s16 *dst = s16_at(w1), *src = s16_at(w1 >> 16);
            for (u32 n = align(st.count, 32) >> 1; n; n--, dst++, src++) *dst = mix(*dst, *src, gain);
            break; }
        case 13: { /* INTERLEAVE */
            s16 *dst = s16_at(st.out), *l = s16_at(w1 >> 16), *r = s16_at(w1);
            s16 tmp[0x400];
            u32 n = st.count >> 1;
            if (n > 0x200) n = 0x200;
            for (u32 k = 0; k < n; k++) { tmp[k * 2] = l[k]; tmp[k * 2 + 1] = r[k]; }
            memcpy(dst, tmp, n * 4);
            break; }
        case 14: polef(flags & A_INIT, st.out, st.in, st.count, (s16)w0, w1); break;
        case 15: st.loop = w1; break; /* SETLOOP */
        default: Port_HostFatal("aspMain: unknown command");
        }
    }
}

u8 *Port_AspDmem(void) { return dmem; }

void Port_RunAudioTask(const OSTask *task) {
    const u32 *cmd = (const u32 *)task->t.data_ptr;
    u32 words = task->t.data_size / 4;
    for (u32 i = 0; i + 1 < words; i += 2) Port_AspCommand(task, cmd[i], cmd[i + 1]);
}
