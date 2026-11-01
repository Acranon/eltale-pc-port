/* nd_model.c: the model loader (nd_model.h). PC-only, game side (in the exe: it reads the game's
 * memory where the game keeps it, RDRAM as N64 bytes and native objects in host order).
 *
 * What it reproduces, per F3DEX 1.23 and the RDP:
 * - the RSP: segments, DL calls and branches, CULLDL (the box test: ignored, the GPU clips), the
 *   32-entry vertex cache filled by VTX with the geometry mode and TEXTURE scale of that moment
 *   (lighting, texgen and fog are worked out per draw from the vertex's normal), TRI1/TRI2;
 * - the RDP state a triangle is drawn with: SETCOMBINE, othermodes, the colours, the 8 tiles and
 *   their sizes, TMEM as the RDP fills it (LOADBLOCK with dxt, LOADTILE, LOADTLUT, odd rows swapped)
 *   and reads it (texel formats RGBA16/32, CI4/8, IA4/8/16, I4/8).
 * Each texture tile a triangle samples is decoded from TMEM over the area the tile can address (its
 * mask, or its clamp extent) and uploaded once (shared by content). CI textures keep their indices:
 * the palette is looked up per frame (nd_model_frame_end).
 *
 * ELTALE_ND_LOG=1 prints a line per mesh made and anything the walk didn't expect. */
#include "game/native_draw/nd_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uintptr_t Port_N64ToNative(uint32_t n64);
void nd_2d_reset(void);

enum { RDRAM = 0x80000000u, RDRAM_BYTES = 8 << 20, MAX_DEPTH = 18, MAX_COMMANDS = 1 << 16 };

static const ElRenderApi *api;
static int log_on = -1;

#define ND_LOG(...) do { if (log_on) fprintf(stderr, __VA_ARGS__); } while (0)

/* ---- memory ------------------------------------------------------------------------------------- */

typedef struct { const uint8_t *p; int native; } Ptr;

static uint32_t segments[16];

static int is_rdram(uint32_t a) { return a >= RDRAM && a < RDRAM + RDRAM_BYTES; }

/* An address word as the RSP would read it: an N64 (KSEG0) address, a segmented or physical one, or a
 * native pointer (the adapter's entry lists, the game's native objects). An N64 address that names a
 * native object (Port_N64ToNative: the main segment's .bss) is that object. */
static Ptr resolve(uint32_t w) {
    Ptr r;
    if (w < 0x10000000u) {
        uint32_t a = segments[w >> 24 & 0xF] + (w & 0x00FFFFFF);
        if (a < RDRAM_BYTES) a |= RDRAM;
        w = a;
    }
    if (is_rdram(w)) {
        uintptr_t n = Port_N64ToNative(w);
        r.p = n ? (const uint8_t *)n : (const uint8_t *)(uintptr_t)w;
        r.native = n != 0;
    } else {
        r.p = (const uint8_t *)(uintptr_t)w;
        r.native = 1;
    }
    return r;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* ---- the mesh being made ------------------------------------------------------------------------ */

typedef struct { uintptr_t lo, hi; } Range;

struct NdMesh {
    uint32_t dl;
    const void *entry;
    uint32_t id;
    Range *ranges;
    int range_count;
    uint16_t *palettes;
    int palette_count;
    NdMesh *next;
};

static struct {
    ElRdVertex *v;
    uint32_t count, cap;
    ElRdGroup *g;
    uint32_t group_count, group_cap;
    ElRdMaterial *m;
    uint32_t material_count, material_cap;
    Range *ranges;
    int range_count, range_cap;
    uint16_t palettes[64];
    int palette_count;
    float bounds[6];
    int bad;
} out;

static void *grow(void *p, uint32_t *cap, uint32_t need, size_t size) {
    if (need <= *cap) return p;
    uint32_t n = *cap ? *cap : 64;
    while (n < need) n *= 2;
    p = realloc(p, n * size);
    if (!p) { fprintf(stderr, "nd_model: out of memory\n"); abort(); }
    *cap = n;
    return p;
}

/* Remember that the mesh read [p, p + bytes) (DMA over it drops the mesh) */
static void note_range(const void *p, uint32_t bytes) {
    uintptr_t lo = (uintptr_t)p, hi = lo + bytes;
    for (int i = 0; i < out.range_count; i++) {
        Range *r = &out.ranges[i];
        if (lo <= r->hi + 64 && hi + 64 >= r->lo) {
            if (lo < r->lo) r->lo = lo;
            if (hi > r->hi) r->hi = hi;
            return;
        }
    }
    uint32_t cap = (uint32_t)out.range_cap;
    out.ranges = grow(out.ranges, &cap, (uint32_t)out.range_count + 1, sizeof(Range));
    out.range_cap = (int)cap;
    out.ranges[out.range_count].lo = lo;
    out.ranges[out.range_count].hi = hi;
    out.range_count++;
}

/* ---- RSP and RDP state ------------------------------------------------------------------------- */

typedef struct {
    uint8_t fmt, siz, palette, cms, cmt, masks, maskt, shifts, shiftt;
    uint16_t line, tmem;
    uint16_t sl, tl, sh, th;
    uint8_t size_from_entry; /* SETTILESIZE came from the entry list */
} Tile;

typedef struct {
    ElRdVertex v;
    int set;
} CacheVertex;

static struct {
    uint32_t geometry, othermode_h, othermode_l, combine[2];
    uint8_t prim[4], env[4], blend[4], fog[4];
    uint32_t own;
    uint8_t prim_lod_frac, k4, k5;
    uint16_t tex_scale[2];
    uint8_t tex_tile, tex_on;
    Tile tile[8];
    struct { uint32_t word; Ptr ptr; uint8_t fmt, siz; uint16_t width; } timg;
    uint8_t tmem[4096];
    /* the TLUT in TMEM's upper half: each entry's source (or none) and its value when loaded */
    const uint8_t *tlut_src[256];
    uint8_t tlut_native[256];
    uint16_t tlut_val[256];
    CacheVertex vtx[32];
    int in_entry;
    /* a LOADBLOCK from native memory waits for the render tile's texel size (see frame_image.c) */
    struct { const uint8_t *src; uint32_t tmem, words, dxt; int pending; } block;
} st;

/* ---- TMEM ---------------------------------------------------------------------------------------- */

/* One 64-bit TMEM word from source bytes in N64 order; odd lines have their 32-bit halves swapped. */
static void tmem_put(uint32_t word, const uint8_t src[8], int odd) {
    uint8_t *d = &st.tmem[(word & 0x1FF) * 8];
    if (odd) { memcpy(d, src + 4, 4); memcpy(d + 4, src, 4); }
    else memcpy(d, src, 8);
}

/* Source bytes of a texture in N64 order: RDRAM is already; a native object is host order by its
 * texel size (unit 2: halfwords, 4: words) */
static void fetch_bytes(const uint8_t *p, int native, int unit, uint32_t offset, uint8_t out8[8]) {
    if (!native || unit == 1) { memcpy(out8, p + offset, 8); return; }
    for (int i = 0; i < 8; i += unit) {
        uint32_t o = offset + (uint32_t)i;
        uint32_t base = o & ~(uint32_t)(unit - 1);
        if (unit == 2) {
            uint16_t v; memcpy(&v, p + base, 2);
            out8[i] = (uint8_t)(v >> 8); out8[i + 1] = (uint8_t)v;
        } else {
            uint32_t v; memcpy(&v, p + base, 4);
            out8[i] = (uint8_t)(v >> 24); out8[i + 1] = (uint8_t)(v >> 16); out8[i + 2] = (uint8_t)(v >> 8); out8[i + 3] = (uint8_t)v;
        }
    }
}

static int unit_of(int siz) { return siz == 2 ? 2 : siz == 3 ? 4 : 1; }

static void do_block(const uint8_t *src, int native, int unit, uint32_t tmem, uint32_t words, uint32_t dxt) {
    for (uint32_t i = 0; i < words; i++) {
        uint8_t b[8];
        fetch_bytes(src, native, unit, i * 8, b);
        uint32_t line = (i * dxt) >> 11;
        tmem_put(tmem + i, b, line & 1);
    }
}

static void flush_block(int siz) {
    if (!st.block.pending) return;
    do_block(st.block.src, 1, unit_of(siz), st.block.tmem, st.block.words, st.block.dxt);
    st.block.pending = 0;
}

static void load_block(const Tile *t, uint32_t lrs, uint32_t dxt) {
    uint32_t texels = lrs + 1;
    uint32_t bytes = t->siz == 0 ? (texels + 1) / 2 : texels << (t->siz - 1);
    uint32_t words = (bytes + 7) / 8;
    if (!st.timg.ptr.p) return;
    note_range(st.timg.ptr.p, words * 8);
    if (st.timg.ptr.native && t->siz == 2) {
        /* a 4- or 8-bit native texture loaded as 16-bit: its real size comes with the render tile */
        flush_block(2);
        st.block.src = st.timg.ptr.p; st.block.tmem = t->tmem; st.block.words = words; st.block.dxt = dxt;
        st.block.pending = 1;
        return;
    }
    do_block(st.timg.ptr.p, st.timg.ptr.native, unit_of(t->siz), t->tmem, words, dxt);
}

static void load_tile(const Tile *t, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    if (!st.timg.ptr.p) return;
    uint32_t s0 = uls >> 2, t0 = ult >> 2, s1 = lrs >> 2, t1 = lrt >> 2;
    int siz = st.timg.siz;
    uint32_t row_texels = s1 - s0 + 1;
    uint32_t row_bytes = siz == 0 ? (row_texels + 1) / 2 : row_texels << (siz - 1);
    uint32_t img_row = siz == 0 ? (st.timg.width + 1u) / 2 : (uint32_t)st.timg.width << (siz - 1);
    uint32_t start = siz == 0 ? s0 / 2 : s0 << (siz - 1);
    int unit = unit_of(siz);
    for (uint32_t r = 0; t0 + r <= t1; r++) {
        uint32_t src_off = (t0 + r) * img_row + start;
        note_range(st.timg.ptr.p + src_off, row_bytes);
        uint32_t words = (row_bytes + 7) / 8;
        for (uint32_t i = 0; i < words; i++) {
            uint8_t b[8];
            fetch_bytes(st.timg.ptr.p, st.timg.ptr.native, unit, src_off + i * 8, b);
            tmem_put(t->tmem + r * t->line + i, b, r & 1);
        }
    }
}

static void load_tlut(const Tile *t, uint32_t uls, uint32_t lrs) {
    if (!st.timg.ptr.p) return;
    uint32_t first = uls >> 2, count = (lrs >> 2) - first + 1;
    int base = (int)t->tmem - 0x100;
    note_range(st.timg.ptr.p + first * 2, count * 2);
    for (uint32_t k = 0; k < count; k++) {
        int e = base + (int)k;
        if (e < 0 || e >= 256) continue;
        const uint8_t *p = st.timg.ptr.p + (first + k) * 2;
        uint16_t v;
        if (st.timg.ptr.native) memcpy(&v, p, 2);
        else v = be16(p);
        st.tlut_src[e] = p;
        st.tlut_native[e] = (uint8_t)st.timg.ptr.native;
        st.tlut_val[e] = v;
        /* (the RDP keeps it quadrupled in the upper half; textures never read it as texels here) */
        uint8_t q[8] = { (uint8_t)(v >> 8), (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)v,
                         (uint8_t)(v >> 8), (uint8_t)v };
        memcpy(&st.tmem[0x800 + (uint32_t)e * 8], q, 8);
    }
}

/* ---- textures ---------------------------------------------------------------------------------- */

static uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; }
static uint32_t c5(uint32_t v) { return v << 3 | v >> 2; }

/* A texel of a tile at (s, t) in its own coordinates (after mask/clamp): RGBA8, or an index (CI). */
static uint32_t tmem_texel(const Tile *t, uint32_t s, uint32_t tt, int ci, uint32_t wrap) {
    uint32_t base = (uint32_t)t->tmem * 8 + tt * (uint32_t)t->line * 8;
    uint32_t odd = (tt & 1) ? 4 : 0;
    switch (t->siz) {
    case 0: {
        uint32_t a = ((base + s / 2) ^ odd) & wrap;
        uint32_t v = (s & 1) ? st.tmem[a] & 0xF : st.tmem[a] >> 4;
        if (ci) return v; /* CI4: 0..15 within the tile's palette */
        if (t->fmt == 3) { uint32_t i = v >> 1; i = i << 5 | i << 2 | i >> 1; return rgba(i, i, i, (v & 1) ? 255 : 0); }
        v = v << 4 | v;
        return rgba(v, v, v, v);
    }
    case 1: {
        uint32_t v = st.tmem[((base + s) ^ odd) & wrap];
        if (ci) return v;
        if (t->fmt == 3) { uint32_t i = (v >> 4) * 17, a = (v & 15) * 17; return rgba(i, i, i, a); }
        return rgba(v, v, v, v);
    }
    case 2: {
        uint32_t a = ((base + s * 2) ^ odd) & wrap;
        uint32_t v = (uint32_t)st.tmem[a] << 8 | st.tmem[(a + 1) & wrap];
        if (ci) return v & 0xFF;
        if (t->fmt == 3) return rgba(v >> 8, v >> 8, v >> 8, v & 0xFF);
        return rgba(c5(v >> 11 & 31), c5(v >> 6 & 31), c5(v >> 1 & 31), (v & 1) ? 255 : 0);
    }
    default: { /* 32-bit: red/green in the lower half, blue/alpha in the upper */
        uint32_t a = ((base + s * 2) ^ odd) & 0x7FF;
        return rgba(st.tmem[a], st.tmem[a + 1], st.tmem[a + 0x800], st.tmem[a + 0x801]);
    }
    }
}

/* Texture cache by content (shared by every mesh) */
typedef struct TexEntry { uint64_t hash; uint32_t offset; struct TexEntry *next; } TexEntry;
static TexEntry *tex_table[4096];
static uint32_t *scratch;
static uint32_t scratch_cap;
static int resources_full;

static uint64_t fnv(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * UINT64_C(0x100000001B3);
    return h;
}

static uint32_t texture_offset(const uint32_t *texels, uint32_t count) {
    uint64_t h = fnv(texels, count * 4u, fnv(&count, 4, UINT64_C(0xCBF29CE484222325)));
    TexEntry **slot = &tex_table[h & 4095];
    for (TexEntry *e = *slot; e; e = e->next)
        if (e->hash == h) return e->offset;
    uint32_t off = api->texture_upload(texels, count);
    if (off == ~0u) { resources_full = 1; return 0; }
    TexEntry *e = malloc(sizeof *e);
    e->hash = h; e->offset = off; e->next = *slot;
    *slot = e;
    return off;
}

/* Palettes: a palette is where its entries were loaded from (runs of memory); each frame the drawn
 * ones are read again. Entries never loaded keep the value they had (a static copy). */
typedef struct {
    uint16_t count;
    uint8_t runs;
    struct { uint16_t first, len; const uint8_t *src; uint8_t native; } run[4];
    uint16_t fixed[256];  /* entries without a source */
    uint16_t sent[256];
    uint8_t has_sent;
    uint64_t key;
} Palette;
static Palette *palettes;
static uint32_t palette_count, palette_cap;
static uint8_t *palette_used;

static uint16_t palette_id(int first_entry, int count) {
    Palette p;
    memset(&p, 0, sizeof p);
    p.count = (uint16_t)count;
    for (int k = 0; k < count; k++) {
        int e = first_entry + k;
        const uint8_t *src = st.tlut_src[e];
        if (!src) { p.fixed[k] = st.tlut_val[e]; continue; }
        if (p.runs && p.run[p.runs - 1].native == st.tlut_native[e] &&
            p.run[p.runs - 1].src + 2 * p.run[p.runs - 1].len == src && p.run[p.runs - 1].first + p.run[p.runs - 1].len == k) {
            p.run[p.runs - 1].len++;
        } else if (p.runs < 4) {
            p.run[p.runs].first = (uint16_t)k; p.run[p.runs].len = 1; p.run[p.runs].src = src;
            p.run[p.runs].native = st.tlut_native[e];
            p.runs++;
        } else {
            p.fixed[k] = st.tlut_val[e]; /* too scattered: keep the value */
        }
    }
    p.key = fnv(&p.count, sizeof p.count, UINT64_C(0xCBF29CE484222325));
    p.key = fnv(&p.runs, 1, p.key);
    for (int r = 0; r < p.runs; r++) p.key = fnv(&p.run[r], sizeof p.run[r], p.key);
    p.key = fnv(p.fixed, sizeof p.fixed, p.key);
    for (uint32_t i = 0; i < palette_count; i++)
        if (palettes[i].key == p.key) return (uint16_t)i;
    if (palette_count >= 4096) { resources_full = 1; return 0; }
    palettes = grow(palettes, &palette_cap, palette_count + 1, sizeof(Palette));
    uint32_t cap = palette_cap;
    palette_used = realloc(palette_used, cap);
    palettes[palette_count] = p;
    palette_used[palette_count] = 0;
    return (uint16_t)palette_count++;
}

static void palette_read(const Palette *p, uint16_t *v) {
    memcpy(v, p->fixed, sizeof p->fixed);
    for (int r = 0; r < p->runs; r++)
        for (int k = 0; k < p->run[r].len; k++) {
            const uint8_t *s = p->run[r].src + 2 * k;
            uint16_t x;
            if (p->run[r].native) memcpy(&x, s, 2);
            else x = be16(s);
            v[p->run[r].first + k] = x;
        }
}

/* A tile decoded over what it can address (ElRdTile) */
static void decode_tile(int index, ElRdTile *o) {
    const Tile *t = &st.tile[index & 7];
    flush_block(t->siz);
    memset(o, 0, sizeof *o);
    int tlut = (st.othermode_h >> 14 & 3) != 0;
    int ci = t->fmt == 2 || (tlut && t->siz <= 1);
    uint32_t cw = (uint32_t)((t->sh >> 2) - (t->sl >> 2) + 1), ch = (uint32_t)((t->th >> 2) - (t->tl >> 2) + 1);
    uint32_t w = t->masks ? 1u << t->masks : cw, h = t->maskt ? 1u << t->maskt : ch;
    if (w < 1 || w > 1024 || h < 1 || h > 1024) {
        ND_LOG("nd_model: tile %d of %ux%u (sizes %d..%d %d..%d) skipped\n", index, w, h, t->sl, t->sh, t->tl, t->th);
        out.bad++;
        return;
    }
    scratch = grow(scratch, &scratch_cap, w * h, 4);
    uint32_t wrap = ci && tlut ? 0x7FF : 0xFFF;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) scratch[y * w + x] = tmem_texel(t, x, y, ci, wrap);
    o->texels = texture_offset(scratch, w * h);
    o->width = (uint16_t)w;
    o->height = (uint16_t)h;
    o->format = ci ? EL_RDT_CI : EL_RDT_RGBA;
    o->cms = (uint8_t)(t->cms | (t->masks ? 0 : 2));
    o->cmt = (uint8_t)(t->cmt | (t->maskt ? 0 : 2));
    o->masks = t->masks; o->maskt = t->maskt; o->shifts = t->shifts; o->shiftt = t->shiftt;
    o->sl = (int16_t)t->sl; o->tl = (int16_t)t->tl; o->sh = (int16_t)t->sh; o->th = (int16_t)t->th;
    if (ci) {
        /* CI4: the tile's 16 entries; CI8: all 256 */
        int first = t->siz == 0 ? t->palette * 16 : 0, count = t->siz == 0 ? 16 : 256;
        o->palette = palette_id(first, count);
        int seen = 0;
        for (int i = 0; i < out.palette_count; i++) seen |= out.palettes[i] == o->palette;
        if (!seen && out.palette_count < 64) out.palettes[out.palette_count++] = o->palette;
    }
}

/* Which texels the combiner reads: bit 0 TEXEL0, bit 1 TEXEL1 (by the cycle that names them; in the
 * second cycle the RDP swaps the two). */
static int combiner_texels(void) {
    uint32_t w0 = st.combine[0], w1 = st.combine[1];
    int two = (st.othermode_h >> 20 & 3) == 1;
    int sel[2][8] = {
        { (int)(w0 >> 20 & 15), (int)(w1 >> 28 & 15), (int)(w0 >> 15 & 31), (int)(w1 >> 15 & 7),
          (int)(w0 >> 12 & 7), (int)(w1 >> 12 & 7), (int)(w0 >> 9 & 7), (int)(w1 >> 9 & 7) },
        { (int)(w0 >> 5 & 15), (int)(w1 >> 24 & 15), (int)(w0 & 31), (int)(w1 >> 6 & 7),
          (int)(w1 >> 21 & 7), (int)(w1 >> 3 & 7), (int)(w1 >> 18 & 7), (int)(w1 & 7) },
    };
    int used = 0;
    for (int c = 0; c < (two ? 2 : 1); c++) {
        int t0 = 0, t1 = 0;
        for (int i = 0; i < 8; i++) {
            int v = sel[c][i];
            if (v == 1 || (i == 2 && v == 8)) t0 = 1;
            if (v == 2 || (i == 2 && v == 9)) t1 = 1;
        }
        if (c == 1) { int x = t0; t0 = t1; t1 = x; }
        used |= t0 | t1 << 1;
    }
    return used;
}

/* ---- triangles -------------------------------------------------------------------------------- */

static ElRdMaterial cur_material;
static int cur_material_valid;

static void make_material(ElRdMaterial *m) {
    memset(m, 0, sizeof *m);
    m->combine[0] = st.combine[0] & 0xFFFFFF;
    m->combine[1] = st.combine[1];
    m->othermode_h = st.othermode_h;
    m->othermode_l = st.othermode_l;
    m->geometry = st.geometry;
    memcpy(m->prim, st.prim, 4); memcpy(m->env, st.env, 4); memcpy(m->blend, st.blend, 4); memcpy(m->fog, st.fog, 4);
    m->own = st.own;
    m->prim_lod_frac = st.prim_lod_frac; m->k4 = st.k4; m->k5 = st.k5;
    m->tile_index = st.tex_tile;
    if (st.tex_on) {
        m->flags |= EL_RDM_TEXTURE;
        int used = combiner_texels();
        if (used & 1) decode_tile(st.tex_tile, &m->tile[0]);
        if (used & 2) decode_tile(st.tex_tile + 1, &m->tile[1]);
        if ((used & 1) && st.tile[st.tex_tile & 7].size_from_entry && !st.in_entry) m->flags |= EL_RDM_TILE_SCROLL;
    }
}

/* the state changed since the last material: made again at the next triangle */
static void state_changed(void) { cur_material_valid = 0; }

static uint32_t material_index(void) {
    if (!cur_material_valid) {
        make_material(&cur_material);
        cur_material_valid = 1;
    }
    for (uint32_t i = out.material_count; i-- > 0;)
        if (!memcmp(&out.m[i], &cur_material, sizeof cur_material)) return i;
    out.m = grow(out.m, &out.material_cap, out.material_count + 1, sizeof(ElRdMaterial));
    out.m[out.material_count] = cur_material;
    return out.material_count++;
}

static void triangle(int a, int b, int c) {
    if (a >= 32 || b >= 32 || c >= 32 || !st.vtx[a].set || !st.vtx[b].set || !st.vtx[c].set) {
        ND_LOG("nd_model: triangle with unloaded vertices %d %d %d\n", a, b, c);
        out.bad++;
        return;
    }
    uint32_t mat = material_index();
    if (!out.group_count || out.g[out.group_count - 1].material != mat ||
        out.g[out.group_count - 1].first + out.g[out.group_count - 1].count != out.count) {
        out.g = grow(out.g, &out.group_cap, out.group_count + 1, sizeof(ElRdGroup));
        out.g[out.group_count].first = out.count;
        out.g[out.group_count].count = 0;
        out.g[out.group_count].material = mat;
        out.group_count++;
    }
    out.v = grow(out.v, &out.cap, out.count + 3, sizeof(ElRdVertex));
    const int idx[3] = { a, b, c };
    int flat = !(st.geometry & 0x200); /* G_SHADING_SMOOTH */
    for (int k = 0; k < 3; k++) {
        ElRdVertex v = st.vtx[idx[k]].v;
        if (flat) { memcpy(v.c, st.vtx[a].v.c, 4); v.flags = (v.flags & ~(uint32_t)(EL_RDV_LIGHT)) | (st.vtx[a].v.flags & EL_RDV_LIGHT); }
        out.v[out.count++] = v;
        const float p[3] = { v.x, v.y, v.z };
        for (int i = 0; i < 3; i++) {
            if (p[i] < out.bounds[i]) out.bounds[i] = p[i];
            if (p[i] > out.bounds[3 + i]) out.bounds[3 + i] = p[i];
        }
    }
    out.g[out.group_count - 1].count += 3;
}

static void load_vertices(Ptr p, uint32_t n, uint32_t v0) {
    note_range(p.p, n * 16);
    for (uint32_t i = 0; i < n; i++) {
        if (v0 + i >= 32) { out.bad++; break; }
        const uint8_t *s = p.p + i * 16;
        int16_t x, y, z, ss, tt;
        if (p.native) {
            memcpy(&x, s, 2); memcpy(&y, s + 2, 2); memcpy(&z, s + 4, 2); memcpy(&ss, s + 8, 2); memcpy(&tt, s + 10, 2);
        } else {
            x = (int16_t)be16(s); y = (int16_t)be16(s + 2); z = (int16_t)be16(s + 4);
            ss = (int16_t)be16(s + 8); tt = (int16_t)be16(s + 10);
        }
        ElRdVertex *v = &st.vtx[v0 + i].v;
        v->x = x; v->y = y; v->z = z;
        /* the RSP's s * scale >> 16 (S10.5), in texels */
        v->s = (float)(((int32_t)ss * (int32_t)st.tex_scale[0]) >> 16) / 32.0f;
        v->t = (float)(((int32_t)tt * (int32_t)st.tex_scale[1]) >> 16) / 32.0f;
        memcpy(v->c, s + 12, 4);
        uint32_t f = 0;
        if (st.geometry & 0x20000) f |= EL_RDV_LIGHT;
        if (st.geometry & 0x40000) f |= EL_RDV_TEXGEN;
        if (st.geometry & 0x80000) f |= EL_RDV_TEXGEN_LIN;
        if (st.geometry & 0x10000) f |= EL_RDV_FOG;
        if (st.geometry & 0x4) f |= EL_RDV_SHADE;
        v->flags = f;
        v->scale[0] = st.tex_scale[0];
        v->scale[1] = st.tex_scale[1];
        st.vtx[v0 + i].set = 1;
    }
}

/* ---- the walk ---------------------------------------------------------------------------------- */

static void set_color(uint8_t *dst, uint32_t w1, uint32_t own_bit) {
    dst[0] = (uint8_t)(w1 >> 24); dst[1] = (uint8_t)(w1 >> 16); dst[2] = (uint8_t)(w1 >> 8); dst[3] = (uint8_t)w1;
    if (st.in_entry) st.own &= ~own_bit; /* the entry list's colour stands for the draw's */
    else st.own |= own_bit;
}

static void walk(uint32_t word, int depth) {
    if (depth > MAX_DEPTH) { out.bad++; return; }
    Ptr p = resolve(word);
    if (!p.p) { out.bad++; return; }
    for (uint32_t i = 0; i < MAX_COMMANDS; i++) {
        const uint8_t *c = p.p + i * 8;
        uint32_t w0, w1;
        if (p.native) { memcpy(&w0, c, 4); memcpy(&w1, c + 4, 4); }
        else { w0 = be32(c); w1 = be32(c + 4); }
        if (!p.native || !st.in_entry) note_range(c, 8);
        uint8_t op = (uint8_t)(w0 >> 24);
        switch (op) {
        case 0x00: break; /* SPNOOP */
        case 0x01: ND_LOG("nd_model: MTX in a model list (ignored)\n"); break;
        case 0x03: ND_LOG("nd_model: MOVEMEM %02x in a model list (ignored)\n", w0 >> 16 & 0xFF); break;
        case 0x04: load_vertices(resolve(w1), w0 >> 10 & 0x3F, (w0 >> 16 & 0xFF) / 2); break;
        case 0x06:
            if ((w0 >> 16 & 0xFF) == 1) { walk(w1, depth + 1); return; }
            walk(w1, depth + 1);
            break;
        case 0xB1: /* TRI2 */
            triangle((int)(w0 >> 16 & 0xFF) / 2, (int)(w0 >> 8 & 0xFF) / 2, (int)(w0 & 0xFF) / 2);
            triangle((int)(w1 >> 16 & 0xFF) / 2, (int)(w1 >> 8 & 0xFF) / 2, (int)(w1 & 0xFF) / 2);
            break;
        case 0xBF: triangle((int)(w1 >> 16 & 0xFF) / 2, (int)(w1 >> 8 & 0xFF) / 2, (int)(w1 & 0xFF) / 2); break;
        case 0xB8: return; /* ENDDL */
        case 0xBE: break;  /* CULLDL: the box test; the GPU clips what's off screen */
        case 0xB6: st.geometry &= ~w1; state_changed(); break;
        case 0xB7: st.geometry |= w1; state_changed(); break;
        case 0xB9: case 0xBA: {
            uint32_t shift = w0 >> 8 & 0xFF, len = w0 & 0xFF;
            uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
            uint32_t *m = op == 0xB9 ? &st.othermode_l : &st.othermode_h;
            *m = (*m & ~mask) | (w1 & mask);
            state_changed();
            break; }
        case 0xEF: st.othermode_h = w0 & 0xFFFFFF; st.othermode_l = w1; state_changed(); break;
        case 0xBB: /* TEXTURE */
            st.tex_scale[0] = (uint16_t)(w1 >> 16); st.tex_scale[1] = (uint16_t)w1;
            st.tex_tile = (uint8_t)(w0 >> 8 & 7); st.tex_on = (uint8_t)(w0 & 1);
            state_changed();
            break;
        case 0xBC: /* MOVEWORD */
            if ((w0 & 0xFF) == 6) segments[(w0 >> 8 & 0xFFFF) / 4 & 0xF] = w1;
            else ND_LOG("nd_model: MOVEWORD %u in a model list (ignored)\n", w0 & 0xFF);
            break;
        case 0xB0: case 0xB2: case 0xB3: case 0xB4: break; /* BRANCH_Z (unused), RDPHALFs */
        case 0xBD: break; /* POPMTX */
        case 0xE4: case 0xE5: ND_LOG("nd_model: TEXRECT in a model list (ignored)\n"); i += 2; break;
        case 0xE6: case 0xE7: case 0xE8: case 0xE9: break; /* syncs */
        case 0xF7: case 0xF6: case 0xFE: case 0xFF: break; /* fill colour/rect, images */
        case 0xFA: /* PRIMCOLOR */
            st.prim_lod_frac = (uint8_t)w0;
            set_color(st.prim, w1, EL_RDM_OWN_PRIM);
            state_changed();
            break;
        case 0xFB: set_color(st.env, w1, EL_RDM_OWN_ENV); state_changed(); break;
        case 0xF9: set_color(st.blend, w1, EL_RDM_OWN_BLEND); state_changed(); break;
        case 0xF8: set_color(st.fog, w1, EL_RDM_OWN_FOG); state_changed(); break;
        case 0xFC: st.combine[0] = w0; st.combine[1] = w1; state_changed(); break;
        case 0xEE: break; /* prim depth */
        case 0xFD: /* SETTIMG */
            st.timg.word = w1; st.timg.ptr = resolve(w1);
            st.timg.fmt = (uint8_t)(w0 >> 21 & 7); st.timg.siz = (uint8_t)(w0 >> 19 & 3);
            st.timg.width = (uint16_t)((w0 & 0xFFF) + 1);
            break;
        case 0xF5: { /* SETTILE */
            Tile *t = &st.tile[w1 >> 24 & 7];
            t->fmt = (uint8_t)(w0 >> 21 & 7); t->siz = (uint8_t)(w0 >> 19 & 3);
            t->line = (uint16_t)(w0 >> 9 & 0x1FF); t->tmem = (uint16_t)(w0 & 0x1FF);
            t->palette = (uint8_t)(w1 >> 20 & 15);
            t->cmt = (uint8_t)(w1 >> 18 & 3); t->maskt = (uint8_t)(w1 >> 14 & 15); t->shiftt = (uint8_t)(w1 >> 10 & 15);
            t->cms = (uint8_t)(w1 >> 8 & 3); t->masks = (uint8_t)(w1 >> 4 & 15); t->shifts = (uint8_t)(w1 & 15);
            if ((w1 >> 24 & 7) != 7) flush_block(t->siz);
            state_changed();
            break; }
        case 0xF2: { /* SETTILESIZE */
            Tile *t = &st.tile[w1 >> 24 & 7];
            t->sl = (uint16_t)(w0 >> 12 & 0xFFF); t->tl = (uint16_t)(w0 & 0xFFF);
            t->sh = (uint16_t)(w1 >> 12 & 0xFFF); t->th = (uint16_t)(w1 & 0xFFF);
            t->size_from_entry = (uint8_t)st.in_entry;
            state_changed();
            break; }
        case 0xF3: load_block(&st.tile[w1 >> 24 & 7], w1 >> 12 & 0xFFF, w1 & 0xFFF); state_changed(); break;
        case 0xF4: load_tile(&st.tile[w1 >> 24 & 7], w0 >> 12 & 0xFFF, w0 & 0xFFF, w1 >> 12 & 0xFFF, w1 & 0xFFF); state_changed(); break;
        case 0xF0: load_tlut(&st.tile[w1 >> 24 & 7], w0 >> 12 & 0xFFF, w1 >> 12 & 0xFFF); state_changed(); break;
        default:
            ND_LOG("nd_model: op %02x (%08x %08x) in a model list (ignored)\n", op, w0, w1);
            break;
        }
    }
    out.bad++;
}

/* ---- the cache ---------------------------------------------------------------------------------- */

static NdMesh *mesh_table[1024];
static uint32_t mesh_count;

void nd_model_init(const ElRenderApi *a) {
    api = a;
    if (log_on < 0) log_on = getenv("ELTALE_ND_LOG") ? atoi(getenv("ELTALE_ND_LOG")) : 0;
}

uint32_t nd_model_id(const NdMesh *mesh) { return mesh->id; }

static void free_mesh(NdMesh *m) {
    free(m->ranges);
    free(m->palettes);
    free(m);
}

void nd_model_reset(void) {
    for (int i = 0; i < 1024; i++) {
        while (mesh_table[i]) { NdMesh *m = mesh_table[i]; mesh_table[i] = m->next; free_mesh(m); }
    }
    for (int i = 0; i < 4096; i++) {
        while (tex_table[i]) { TexEntry *e = tex_table[i]; tex_table[i] = e->next; free(e); }
    }
    palette_count = 0;
    mesh_count = 0;
    nd_2d_reset(); /* the 2D core's textures, materials and palettes go too (nd_2d.c) */
    if (api) api->resources_reset();
}

extern void *gfx_segment1_base;

static NdMesh *convert(uint32_t dl, const void *entry) {
    memset(&st, 0, sizeof st);
    st.othermode_h = 0x00080CFF; /* F3DEX's othermode at the start of a task (then the frame list's) */
    out.count = out.group_count = out.material_count = 0;
    out.range_count = 0;
    out.palette_count = 0;
    out.bad = 0;
    for (int i = 0; i < 3; i++) { out.bounds[i] = 1e30f; out.bounds[3 + i] = -1e30f; }
    cur_material_valid = 0;
    memset(segments, 0, sizeof segments);
    uintptr_t s1 = (uintptr_t)gfx_segment1_base;
    segments[1] = (uint32_t)(s1 >= RDRAM && s1 < RDRAM + RDRAM_BYTES ? s1 - RDRAM : s1);
    if (entry) {
        st.in_entry = 1;
        walk((uint32_t)(uintptr_t)entry, 0);
        st.in_entry = 0;
        state_changed();
    }
    walk(dl, 0);
    if (resources_full) return NULL;
    if (!out.count) {
        for (int i = 0; i < 6; i++) out.bounds[i] = 0;
    }
    ElRdMeshDesc d;
    d.vertices = out.v; d.vertex_count = out.count;
    d.groups = out.g; d.group_count = out.group_count;
    d.materials = out.m; d.material_count = out.material_count;
    memcpy(d.bounds, out.bounds, sizeof d.bounds);
    uint32_t id = api->mesh_upload(&d);
    if (id == ~0u) { resources_full = 1; return NULL; }
    NdMesh *m = calloc(1, sizeof *m);
    m->dl = dl; m->entry = entry; m->id = id;
    m->range_count = out.range_count;
    m->ranges = malloc(sizeof(Range) * (size_t)(out.range_count ? out.range_count : 1));
    memcpy(m->ranges, out.ranges, sizeof(Range) * (size_t)out.range_count);
    m->palette_count = out.palette_count;
    m->palettes = malloc(sizeof(uint16_t) * (size_t)(out.palette_count ? out.palette_count : 1));
    memcpy(m->palettes, out.palettes, sizeof(uint16_t) * (size_t)out.palette_count);
    ND_LOG("nd_model: mesh %u from %08x (entry %p): %u triangles, %u groups, %u materials, %d ranges, %d palettes%s\n", id, dl,
           entry, out.count / 3, out.group_count, out.material_count, out.range_count, out.palette_count,
           out.bad ? " (with problems)" : "");
    if (log_on > 1)
        for (uint32_t i = 0; i < out.material_count; i++) {
            const ElRdMaterial *mm = &out.m[i];
            ND_LOG("  material %u: cc %06x:%08x om %06x:%08x geom %05x own %x prim %02x%02x%02x%02x flags %x", i, mm->combine[0],
                   mm->combine[1], mm->othermode_h, mm->othermode_l, mm->geometry, mm->own, mm->prim[0], mm->prim[1], mm->prim[2],
                   mm->prim[3], mm->flags);
            for (int t = 0; t < 2; t++)
                if (mm->tile[t].format)
                    ND_LOG(" | t%d fmt %d %ux%u cm %d/%d mask %d/%d sz %d,%d..%d,%d pal %u @%u", t, mm->tile[t].format,
                           mm->tile[t].width, mm->tile[t].height, mm->tile[t].cms, mm->tile[t].cmt, mm->tile[t].masks,
                           mm->tile[t].maskt, mm->tile[t].sl, mm->tile[t].tl, mm->tile[t].sh, mm->tile[t].th, mm->tile[t].palette,
                           mm->tile[t].texels);
            ND_LOG("\n");
        }
    return m;
}

const NdMesh *nd_model_get(uint32_t dl, const void *entry) {
    uint32_t h = (dl * 2654435761u ^ (uint32_t)(uintptr_t)entry * 40503u) >> 22;
    for (NdMesh *m = mesh_table[h]; m; m = m->next)
        if (m->dl == dl && m->entry == entry) return m;
    resources_full = 0;
    NdMesh *m = convert(dl, entry);
    if (!m && resources_full) {
        /* the renderer's buffers are full: start them again (the rest load as they are drawn) */
        fprintf(stderr, "nd_model: the renderer's buffers are full: loading everything again\n");
        nd_model_reset();
        resources_full = 0;
        m = convert(dl, entry);
    }
    if (!m) return NULL;
    m->next = mesh_table[h];
    mesh_table[h] = m;
    mesh_count++;
    return m;
}

void nd_model_use(const NdMesh *mesh) {
    for (int i = 0; i < mesh->palette_count; i++) palette_used[mesh->palettes[i]] = 1;
}

void nd_model_frame_end(void) {
    uint16_t v[256];
    for (uint32_t i = 0; i < palette_count; i++) {
        if (!palette_used[i]) continue;
        palette_used[i] = 0;
        Palette *p = &palettes[i];
        palette_read(p, v);
        if (p->has_sent && !memcmp(v, p->sent, p->count * 2u)) continue;
        memcpy(p->sent, v, p->count * 2u);
        p->has_sent = 1;
        api->palette(i, v, p->count);
    }
}

void nd_model_forget(uintptr_t host, uint32_t bytes) {
    uintptr_t lo = host, hi = host + bytes;
    for (int i = 0; i < 1024; i++) {
        for (NdMesh **pm = &mesh_table[i]; *pm;) {
            NdMesh *m = *pm;
            int hit = 0;
            for (int r = 0; r < m->range_count && !hit; r++) hit = m->ranges[r].lo < hi && lo < m->ranges[r].hi;
            if (hit) { *pm = m->next; free_mesh(m); mesh_count--; }
            else pm = &m->next;
        }
    }
}

/* ---- for other loaders (nd_model.h) ---------------------------------------------------------------- */

uint32_t nd_model_texture(const uint32_t *texels, uint32_t count) {
    resources_full = 0;
    uint32_t off = texture_offset(texels, count);
    return resources_full ? ~0u : off;
}

static uint32_t palette_intern(Palette *pp) {
    Palette p = *pp;
    p.key = fnv(&p.count, sizeof p.count, UINT64_C(0xCBF29CE484222325));
    p.key = fnv(&p.runs, 1, p.key);
    for (int r = 0; r < p.runs; r++) p.key = fnv(&p.run[r], sizeof p.run[r], p.key);
    p.key = fnv(p.fixed, sizeof p.fixed, p.key);
    for (uint32_t i = 0; i < palette_count; i++)
        if (palettes[i].key == p.key) { palette_used[i] = 1; return i; }
    if (palette_count >= 4096) return ~0u;
    palettes = grow(palettes, &palette_cap, palette_count + 1, sizeof(Palette));
    palette_used = realloc(palette_used, palette_cap);
    palettes[palette_count] = p;
    palette_used[palette_count] = 1;
    return palette_count++;
}

uint32_t nd_model_palette_values(const uint16_t *entries, int count) {
    Palette p;
    memset(&p, 0, sizeof p);
    p.count = (uint16_t)count;
    memcpy(p.fixed, entries, (size_t)count * 2);
    return palette_intern(&p);
}

uint32_t nd_model_palette_source(const uint8_t *src, int native, int count) {
    Palette p;
    memset(&p, 0, sizeof p);
    p.count = (uint16_t)count;
    p.runs = 1;
    p.run[0].first = 0;
    p.run[0].len = (uint16_t)count;
    p.run[0].src = src;
    p.run[0].native = (uint8_t)native;
    return palette_intern(&p);
}
