/* aspMain (libultra's audio microcode, ABI 1) as C: runs an audio task's command list on the CPU.
 *
 * The command list is built by native code (libultra's synthesizer, compiled for the host), so its
 * words are host-order and its addresses are host addresses below 4 GiB, or KSEG0/physical RDRAM
 * addresses (port/os K0_TO_PHYS). All the buffers it reads and writes (ADPCM and resampler states,
 * envelope states, sample and output buffers) are native too, so DMEM is kept in host order.
 *
 * The algorithms follow the RSP microcode's behaviour as documented by the HLE in
 * mupen64plus-rsp-hle (alist.c, alist_audio.c). The resampler's filter table is the microcode's
 * own, read from the task's ucode data in RDRAM (from the user's ROM). The envelope state layout
 * is our own: only this file reads it. */
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

static s16 *s16_at(u32 off) { return (s16 *)(dmem + (off & 0xFFE)); }
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

static void resample(int init, u16 out, u16 in, u32 count, u32 pitch, u32 state) {
    s16 *sp = dram(state);
    u32 ipos = (in >> 1) - 4, opos = out >> 1, accu;
    s16 *d = (s16 *)dmem;
    if (init) {
        for (int k = 0; k < 4; k++) d[(ipos + k) & 0x7FF] = 0;
        accu = 0;
    } else {
        for (int k = 0; k < 4; k++) d[(ipos + k) & 0x7FF] = sp[k];
        accu = (u16)sp[4];
    }
    for (count >>= 1; count; count--) {
        const s16 *lut = resample_lut + ((accu & 0xFC00) >> 8);
        s32 v = d[ipos & 0x7FF] * lut[0] + d[(ipos + 1) & 0x7FF] * lut[1] + d[(ipos + 2) & 0x7FF] * lut[2] +
                d[(ipos + 3) & 0x7FF] * lut[3];
        d[opos++ & 0x7FF] = clamp16(v >> 15);
        accu += pitch;
        ipos += accu >> 16;
        accu &= 0xFFFF;
    }
    for (int k = 0; k < 4; k++) sp[k] = d[(ipos + k) & 0x7FF];
    sp[4] = (s16)accu;
}

typedef struct { s32 value, target, step; } Ramp;
typedef struct { s16 wet, dry; s32 target[2], rates[2], seq[2], value[2]; } EnvState; /* ours */

static s16 ramp_step(Ramp *r) {
    r->value += r->step;
    int reached = r->step <= 0 ? r->value <= r->target : r->value >= r->target;
    if (reached) { r->value = r->target; r->step = 0; }
    return (s16)(r->value >> 16);
}

static void envmixer(int init, int aux, u32 state) {
    EnvState *sp = dram(state);
    Ramp ramps[2];
    s32 seq[2], rates[2];
    s16 dry = st.dry, wet = st.wet;
    if (init) {
        for (int i = 0; i < 2; i++) {
            ramps[i].value = st.vol[i] << 16;
            ramps[i].target = st.target[i] << 16;
            rates[i] = st.rate[i];
            seq[i] = st.vol[i] * st.rate[i];
        }
    } else {
        wet = sp->wet;
        dry = sp->dry;
        for (int i = 0; i < 2; i++) {
            ramps[i].target = sp->target[i];
            rates[i] = sp->rates[i];
            seq[i] = sp->seq[i];
            ramps[i].value = sp->value[i];
        }
    }
    for (int i = 0; i < 2; i++) ramps[i].step = ramps[i].target - ramps[i].value;
    const s16 *in = s16_at(st.in);
    s16 *dst[4] = { s16_at(st.out), s16_at(st.dry_right), s16_at(st.wet_left), s16_at(st.wet_right) };
    unsigned n = aux ? 4 : 2;
    for (u32 y = 0, ptr = 0; y < st.count; y += 16) {
        for (int i = 0; i < 2; i++) {
            if (ramps[i].step) {
                seq[i] = (s32)(((int64_t)seq[i] * rates[i]) >> 16);
                ramps[i].step = (seq[i] - ramps[i].value) >> 3;
            }
        }
        for (int x = 0; x < 8; x++, ptr++) {
            s16 l = ramp_step(&ramps[0]), r = ramp_step(&ramps[1]);
            s16 g[4] = { clamp16((l * dry + 0x4000) >> 15), clamp16((r * dry + 0x4000) >> 15),
                         clamp16((l * wet + 0x4000) >> 15), clamp16((r * wet + 0x4000) >> 15) };
            for (unsigned k = 0; k < n; k++) dst[k][ptr] = clamp16(dst[k][ptr] + ((in[ptr] * g[k]) >> 15));
        }
    }
    sp->wet = wet;
    sp->dry = dry;
    for (int i = 0; i < 2; i++) {
        sp->target[i] = ramps[i].target;
        sp->rates[i] = rates[i];
        sp->seq[i] = seq[i];
        sp->value[i] = ramps[i].value;
    }
}

static void polef(int init, u16 out, u16 in, u32 count, s16 gain, u32 state) {
    s16 *sp = dram(state);
    s16 *dst = s16_at(out);
    const s16 *h1 = adpcm_table;
    s16 h2[8], h2_before[8];
    s16 l1 = init ? 0 : sp[2], l2 = init ? 0 : sp[3];
    for (int i = 0; i < 8; i++) { h2_before[i] = adpcm_table[8 + i]; h2[i] = (s16)((adpcm_table[8 + i] * gain) >> 14); }
    count = align(count, 16);
    do {
        s16 frame[8];
        for (int i = 0; i < 8; i++, in += 2) frame[i] = *s16_at(in);
        for (int i = 0; i < 8; i++) {
            s32 accu = frame[i] * gain + h1[i] * l1 + h2_before[i] * l2;
            for (int k = 0; k < i; k++) accu += h2[i - 1 - k] * frame[k];
            dst[i] = clamp16(accu >> 14);
        }
        l1 = dst[6];
        l2 = dst[7];
        dst += 8;
        count -= 16;
    } while (count);
    memcpy(sp, dst - 4, 8);
}

void Port_RunAudioTask(const OSTask *task) {
    if (!lut_ready) load_lut((u32)(uptr)task->t.ucode_data);
    const u32 *cmd = (const u32 *)task->t.data_ptr;
    u32 words = task->t.data_size / 4;
    for (u32 i = 0; i + 1 < words; i += 2) {
        u32 w0 = cmd[i], w1 = cmd[i + 1];
        u8 op = w0 >> 24, flags = (w0 >> 16) & 0xFF;
        switch (op) {
        case 0: break; /* SPNOOP */
        case 1: adpcm(flags & A_INIT, flags & A_LOOP, st.out, st.in, align(st.count, 32), w1); break;
        case 2: memset(dmem + (w0 & 0xFFF), 0, align(w1 & 0xFFFF, 16) & 0xFFF); break; /* CLEARBUFF */
        case 3: envmixer(flags & A_INIT, flags & A_AUX, w1); break;
        case 4: if (st.count) memcpy(dmem + (st.in & 0xFFF), dram(w1 & ~3u), st.count); break; /* LOADBUFF */
        case 5: resample(flags & A_INIT, st.out, st.in, align(st.count, 16), (w0 & 0xFFFF) << 1, w1); break;
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
        case 10: memmove(dmem + ((w1 >> 16) & 0xFFF), dmem + (w0 & 0xFFF), align(w1 & 0xFFFF, 4)); break; /* DMEMMOVE */
        case 11: memcpy(adpcm_table, dram(w1), (w0 & 0xFFFF) > sizeof adpcm_table ? sizeof adpcm_table : (w0 & 0xFFFF)); break;
        case 12: { /* MIXER */
            s16 gain = (s16)w0;
            s16 *dst = s16_at(w1), *src = s16_at(w1 >> 16);
            for (u32 n = st.count >> 1; n; n--, dst++, src++) *dst = clamp16(*dst + ((*src * gain) >> 15));
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
