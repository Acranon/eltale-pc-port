/* Instrument banks (.ctl) and the sequence file header, decoded from their big-endian N64 form
 * into native libultra structs. On the N64, alBnkfNew and alSeqFileNew turn the file's offsets
 * into pointers in place; the structs have pointer fields, which are 8 bytes here, so the game
 * (port patch to game/audio/audio.c) uses the copies these return instead.
 *
 * Objects the file shares (one envelope or wavetable used by several sounds) stay shared. Sample
 * data stays in the ROM (wavetable bases are ROM addresses, read by the audio DMA callback). The
 * copies live in a static arena: the ADPCM books and loop states reach the aspMain command lists
 * as 32-bit addresses, so they must be below 4 GiB, like everything in the image. */
#include "port/audio.h"

#include <stdint.h>
#include <string.h>

__attribute__((noreturn)) void Port_HostFatal(const char *what);

enum { ARENA_BYTES = 1 << 20, MAX_SHARED = 4096 };
static _Alignas(16) u8 arena[ARENA_BYTES];
static size_t arena_used;
static struct { const u8 *src; void *copy; } shared[MAX_SHARED];
static unsigned shared_count;

static void *alloc(size_t bytes) {
    size_t at = (arena_used + 15) & ~(size_t)15;
    if (bytes > ARENA_BYTES - at) Port_HostFatal("audio bank arena full");
    arena_used = at + bytes;
    memset(arena + at, 0, bytes);
    return arena + at;
}

static void *find(const u8 *src) {
    for (unsigned i = 0; i < shared_count; i++)
        if (shared[i].src == src) return shared[i].copy;
    return NULL;
}

static void *remember(const u8 *src, void *copy) {
    if (shared_count == MAX_SHARED) Port_HostFatal("audio bank: too many objects");
    shared[shared_count].src = src;
    shared[shared_count++].copy = copy;
    return copy;
}

static u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static u32 be32(const u8 *p) { return (u32)be16(p) << 16 | be16(p + 2); }

static const u8 *file_base;
static u32 wave_table;

static const u8 *at(u32 offset) { return offset ? file_base + offset : NULL; }

static ALEnvelope *envelope(const u8 *p) {
    ALEnvelope *e = find(p);
    if (e || !p) return e;
    e = remember(p, alloc(sizeof *e));
    e->attackTime = (s32)be32(p);
    e->decayTime = (s32)be32(p + 4);
    e->releaseTime = (s32)be32(p + 8);
    e->attackVolume = p[12];
    e->decayVolume = p[13];
    return e;
}

static ALKeyMap *keymap(const u8 *p) {
    ALKeyMap *k = find(p);
    if (k || !p) return k;
    k = remember(p, alloc(sizeof *k));
    memcpy(k, p, 6); /* six bytes */
    return k;
}

static ALADPCMBook *book(const u8 *p) {
    ALADPCMBook *b = find(p);
    if (b || !p) return b;
    s32 order = (s32)be32(p), npred = (s32)be32(p + 4);
    if (order < 0 || order > 8 || npred < 0 || npred > 16) Port_HostFatal("audio bank: implausible ADPCM book");
    u32 n = (u32)(order * npred * 8);
    b = remember(p, alloc(sizeof *b + n * sizeof(s16)));
    b->order = order;
    b->npredictors = npred;
    for (u32 i = 0; i < n; i++) b->book[i] = (s16)be16(p + 8 + i * 2);
    return b;
}

static ALADPCMloop *adpcm_loop(const u8 *p) {
    ALADPCMloop *l = find(p);
    if (l || !p) return l;
    l = remember(p, alloc(sizeof *l));
    l->start = be32(p);
    l->end = be32(p + 4);
    l->count = be32(p + 8);
    for (u32 i = 0; i < 16; i++) l->state[i] = (s16)be16(p + 12 + i * 2);
    return l;
}

static ALRawLoop *raw_loop(const u8 *p) {
    ALRawLoop *l = find(p);
    if (l || !p) return l;
    l = remember(p, alloc(sizeof *l));
    l->start = be32(p);
    l->end = be32(p + 4);
    l->count = be32(p + 8);
    return l;
}

static ALWaveTable *wavetable(const u8 *p) {
    ALWaveTable *w = find(p);
    if (w || !p) return w;
    w = remember(p, alloc(sizeof *w));
    w->base = (u8 *)(uptr)(wave_table + be32(p)); /* a ROM address */
    w->len = (s32)be32(p + 4);
    w->type = p[8];
    w->flags = 1;
    if (w->type == AL_ADPCM_WAVE) {
        w->waveInfo.adpcmWave.loop = adpcm_loop(at(be32(p + 12)));
        w->waveInfo.adpcmWave.book = book(at(be32(p + 16)));
    } else if (w->type == AL_RAW16_WAVE) {
        w->waveInfo.rawWave.loop = raw_loop(at(be32(p + 12)));
    }
    return w;
}

static ALSound *sound(const u8 *p) {
    ALSound *s = find(p);
    if (s || !p) return s;
    s = remember(p, alloc(sizeof *s));
    s->envelope = envelope(at(be32(p)));
    s->keyMap = keymap(at(be32(p + 4)));
    s->wavetable = wavetable(at(be32(p + 8)));
    s->samplePan = p[12];
    s->sampleVolume = p[13];
    s->flags = 1;
    return s;
}

static ALInstrument *instrument(const u8 *p) {
    ALInstrument *in = find(p);
    if (in || !p) return in;
    s16 count = (s16)be16(p + 14);
    if (count < 0 || count > 256) Port_HostFatal("audio bank: implausible sound count");
    in = remember(p, alloc(sizeof *in + (size_t)count * sizeof(ALSound *)));
    in->volume = p[0];
    in->pan = p[1];
    in->priority = p[2];
    in->flags = 1;
    in->tremType = p[4]; in->tremRate = p[5]; in->tremDepth = p[6]; in->tremDelay = p[7];
    in->vibType = p[8]; in->vibRate = p[9]; in->vibDepth = p[10]; in->vibDelay = p[11];
    in->bendRange = (s16)be16(p + 12);
    in->soundCount = count;
    for (s16 i = 0; i < count; i++) in->soundArray[i] = sound(at(be32(p + 16 + i * 4)));
    return in;
}

static ALBank *bank(const u8 *p) {
    ALBank *b = find(p);
    if (b || !p) return b;
    s16 count = (s16)be16(p);
    if (count < 0 || count > 512) Port_HostFatal("audio bank: implausible instrument count");
    b = remember(p, alloc(sizeof *b + (size_t)count * sizeof(ALInstrument *)));
    b->instCount = count;
    b->flags = 1;
    b->sampleRate = (s32)be32(p + 4);
    b->percussion = instrument(at(be32(p + 8)));
    for (s16 i = 0; i < count; i++) b->instArray[i] = instrument(at(be32(p + 12 + i * 4)));
    return b;
}

ALBankFile *Port_BankFileNew(const void *file, const void *table) {
    const u8 *p = file;
    if ((s16)be16(p) != AL_BANK_VERSION) Port_HostFatal("audio bank: wrong revision");
    s16 count = (s16)be16(p + 2);
    if (count < 1 || count > 64) Port_HostFatal("audio bank: implausible bank count");
    file_base = p;
    wave_table = (u32)(uptr)table;
    ALBankFile *f = alloc(sizeof *f + (size_t)count * sizeof(ALBank *));
    f->revision = AL_BANK_VERSION;
    f->bankCount = count;
    for (s16 i = 0; i < count; i++) f->bankArray[i] = bank(at(be32(p + 4 + i * 4)));
    return f;
}

ALSeqFile *Port_SeqFileNew(const void *file, const void *base) {
    const u8 *p = file;
    s16 count = (s16)be16(p + 2);
    if (count < 0 || count > 1024) Port_HostFatal("sequence file: implausible count");
    ALSeqFile *f = alloc(sizeof *f + (size_t)count * sizeof(ALSeqData));
    f->revision = (s16)be16(p);
    f->seqCount = count;
    for (s16 i = 0; i < count; i++) {
        f->seqArray[i].offset = (u8 *)(uptr)((u32)(uptr)base + be32(p + 4 + i * 8)); /* a ROM address */
        f->seqArray[i].len = (s32)be32(p + 8 + i * 8);
    }
    return f;
}
