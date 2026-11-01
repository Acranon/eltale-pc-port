/* nd_2d.c: the native renderer's 2D core (nd_2d.h, docs/renderer/NATIVE_DRAW.md "2D"). PC-only, game
 * side.
 *
 * The state the 2D code sets is kept as the RDP would hold it: othermodes, combiner, the colour
 * registers, the 8 tiles and the TEXTURE command, and TMEM. TMEM is kept by reference: each load
 * remembers which bytes of memory went to which TMEM bytes (a tile load row by row, a block as one run,
 * the TLUT entry by entry), and a tile is decoded from those bytes when a rectangle samples it (once per
 * load: the result is cached until TMEM or the tile changes). A texture rectangle then becomes one
 * ElRd2D: the screen rectangle, s/t at its corner and the steps, a material (combiner, othermodes, the
 * decoded tiles) and the colours. Fill rectangles in FILL mode get a material that writes the fill
 * colour.
 *
 * Static lists (nd_2d_display_list) are read once into calls (Op) and replayed: their state, their
 * rectangles, and their triangles, which the model loader makes a mesh of (entered with the 2D state at
 * the list's call) and which are drawn with the 2D projection (the game's ortho and translate) and the
 * model matrix the code loaded. */
#include "game/native_draw/nd_2d.h"
#include "game/native_draw/nd_internal.h"

#include "game/core/main.h"
#include "game/native_draw/nd_2d_gbi.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uintptr_t Port_N64ToNative(uint32_t n64);
extern void *gfx_segment1_base;

enum { RDRAM = 0x80000000u, RDRAM_BYTES = 8 << 20 };

static int log_on = -1;
#define LOG2D(...) do { if (log_on) fprintf(stderr, __VA_ARGS__); } while (0)

/* ---- memory ------------------------------------------------------------------------------------- */

typedef struct { const uint8_t *p; int native; } Ptr;

static uint8_t *frame_data; /* segment 2 */

static Ptr resolve(uintptr_t w) {
    Ptr r = { NULL, 0 };
    if (w < 0x10000000u) {
        uint32_t seg = (uint32_t)(w >> 24) & 0xF, off = (uint32_t)w & 0xFFFFFF;
        if (seg == 2) {
            if (frame_data) r.p = frame_data + off, r.native = 1;
            return r;
        }
        uintptr_t base = 0;
        if (seg == 1) {
            uintptr_t s1 = (uintptr_t)gfx_segment1_base;
            base = s1 >= RDRAM && s1 < RDRAM + RDRAM_BYTES ? s1 - RDRAM : s1;
        }
        w = base + off;
        if (w < RDRAM_BYTES) w |= RDRAM;
    }
    if (w >= RDRAM && w < RDRAM + RDRAM_BYTES) {
        uintptr_t n = Port_N64ToNative((uint32_t)w);
        r.p = n ? (const uint8_t *)n : (const uint8_t *)w;
        r.native = n != 0;
    } else {
        r.p = (const uint8_t *)w;
        r.native = 1;
    }
    return r;
}

/* A byte of memory in the N64's order: native memory holds `unit`-byte values in host order */
static uint8_t mem_byte(const uint8_t *p, int native, int unit) {
    if (native && unit > 1) p = (const uint8_t *)((uintptr_t)p ^ (uintptr_t)(unit - 1));
    return *p;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* ---- the state ---------------------------------------------------------------------------------- */

typedef struct {
    uint8_t fmt, siz, palette, cms, cmt, masks, maskt, shifts, shiftt;
    uint16_t line, tmem;
    uint16_t sl, tl, sh, th;
} Tile;

/* A TMEM load by reference: TMEM bytes [lo, hi) came from memory */
typedef struct {
    uint32_t lo, hi;
    uint32_t row_tmem;  /* TMEM bytes per row (a tile load); 0: one run (a block) */
    uint32_t row_bytes; /* bytes loaded per row */
    uint32_t rows;
    const uint8_t *src; /* the bytes of TMEM byte lo */
    uint32_t row_src;   /* memory bytes per row */
    uint8_t native, unit;
} Load;

enum { MAX_LOADS = 16 };

static struct {
    uint32_t othermode_h, othermode_l, combine[2], geometry;
    uint8_t env[4], prim[4], blend[4], fog[4];
    uint32_t fill;
    uint8_t prim_lod;
    uint16_t tex_s, tex_t;
    uint8_t tex_level, tex_tile, tex_on;
    Tile tile[8];
    struct { uint8_t fmt, siz; uint32_t width; Ptr p; uintptr_t word; } timg;
    Load load[MAX_LOADS];
    int load_count;
    const uint8_t *tlut_src[256];
    uint8_t tlut_native[256];
    uint32_t tmem_gen; /* bumped by every load */
    float proj[16], mv[16];
} st;

int nd2d_kind = -1;
static int recording;
static int element, anchor;
static uint32_t item_number;

static int kind_default_anchor(int kind) {
    (void)kind;
    return EL_RD2D_ANCHOR_CENTRE;
}

static void identity(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1;
}

void nd_2d_frame(struct GfxFrameData *frame) { frame_data = (uint8_t *)frame; }

void nd_2d_frame_begin(void) {
    if (log_on < 0) log_on = getenv("ELTALE_ND_LOG") ? atoi(getenv("ELTALE_ND_LOG")) : 0;
    /* F3DEX's state at a task's start, then the frame's set-up (2-cycle) */
    memset(&st, 0, sizeof st);
    st.othermode_h = 0x00080CFF;
    st.othermode_h = (st.othermode_h & ~(3u << 20)) | (1u << 20);
    identity(st.proj);
    identity(st.mv);
    nd2d_kind = -1;
    recording = 0;
}

void nd_2d_section(int kind) {
    nd2d_kind = kind;
    recording = kind >= 0 && ND_ON(kind);
    element = 0;
    anchor = kind_default_anchor(kind);
    item_number = 0;
}

void nd_2d_element(int e, int a) {
    element = e;
    anchor = a;
    item_number = 0;
}

void nd_2d_clip_x(int32_t *left, int32_t *right) {
    if (!ND_WIDE() || anchor != EL_RD2D_ANCHOR_WORLD) return;
    /* the 4:3 screen's 320 columns sit in the middle of 240 x aspect: a world-placed item may use the sides */
    const int32_t side = (int32_t)ceilf((240.0f * nd_aspect - 320.0f) * 0.5f);
    if (*left <= 0) *left -= side;
    if (*right >= 320) *right += side;
}

void nd_2d_othermode(uint32_t cmd, uint32_t shift, uint32_t len, uint32_t data) {
    uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
    /* (F3DEX's immediate command numbers are negative ints: compare the low byte) */
    uint32_t *m = (cmd & 0xFF) == (G_SETOTHERMODE_L & 0xFF) ? &st.othermode_l : &st.othermode_h;
    *m = (*m & ~mask) | (data & mask);
}

static void set_rgba(uint8_t *dst, uint32_t v) {
    dst[0] = (uint8_t)(v >> 24); dst[1] = (uint8_t)(v >> 16); dst[2] = (uint8_t)(v >> 8); dst[3] = (uint8_t)v;
}

void nd_2d_color(uint32_t cmd, uint32_t value) {
    if (log_on > 2) fprintf(stderr, "nd_2d: colour %02x %08x\n", cmd & 0xFF, value);
    switch (cmd & 0xFF) {
    case G_SETENVCOLOR: set_rgba(st.env, value); break;
    case G_SETBLENDCOLOR: set_rgba(st.blend, value); break;
    case G_SETFOGCOLOR: set_rgba(st.fog, value); break;
    case G_SETFILLCOLOR: st.fill = value; break;
    default: break;
    }
}

void nd_2d_prim_color(uint32_t m, uint32_t l, uint32_t rgba) {
    (void)m;
    st.prim_lod = (uint8_t)l;
    set_rgba(st.prim, rgba);
}

void nd_2d_combine(uint32_t w0, uint32_t w1) {
    st.combine[0] = w0 & 0xFFFFFF;
    st.combine[1] = w1;
}

void nd_2d_texture_image(uint32_t fmt, uint32_t siz, uint32_t width, uintptr_t address) {
    st.timg.fmt = (uint8_t)fmt;
    st.timg.siz = (uint8_t)siz;
    st.timg.width = width;
    st.timg.word = address;
    st.timg.p = resolve(address);
}

void nd_2d_tile(uint32_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint32_t tile, uint32_t palette, uint32_t cmt,
                uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks, uint32_t shifts) {
    Tile *t = &st.tile[tile & 7];
    t->fmt = (uint8_t)(fmt & 7); t->siz = (uint8_t)(siz & 3);
    t->line = (uint16_t)(line & 0x1FF); t->tmem = (uint16_t)(tmem & 0x1FF);
    t->palette = (uint8_t)(palette & 15);
    t->cmt = (uint8_t)(cmt & 3); t->maskt = (uint8_t)(maskt & 15); t->shiftt = (uint8_t)(shiftt & 15);
    t->cms = (uint8_t)(cms & 3); t->masks = (uint8_t)(masks & 15); t->shifts = (uint8_t)(shifts & 15);
}

void nd_2d_tile_size(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    Tile *t = &st.tile[tile & 7];
    t->sl = (uint16_t)(uls & 0xFFF); t->tl = (uint16_t)(ult & 0xFFF);
    t->sh = (uint16_t)(lrs & 0xFFF); t->th = (uint16_t)(lrt & 0xFFF);
}

static int bytes_per_texel(int siz) { return siz == 0 ? 0 : siz == 1 ? 1 : siz == 2 ? 2 : 4; }

static void add_load(const Load *l) {
    /* drop older loads this one covers entirely */
    for (int i = 0; i < st.load_count;) {
        if (st.load[i].lo >= l->lo && st.load[i].hi <= l->hi) {
            memmove(&st.load[i], &st.load[i + 1], (size_t)(st.load_count - i - 1) * sizeof(Load));
            st.load_count--;
        } else {
            i++;
        }
    }
    if (st.load_count == MAX_LOADS) {
        memmove(&st.load[0], &st.load[1], (MAX_LOADS - 1) * sizeof(Load));
        st.load_count--;
    }
    st.load[st.load_count++] = *l;
    st.tmem_gen++;
}

void nd_2d_load_tile(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    Tile *t = &st.tile[tile & 7];
    nd_2d_tile_size(tile, uls, ult, lrs, lrt);
    if (!st.timg.p.p) return;
    int bpp = bytes_per_texel(st.timg.siz);
    if (!bpp) bpp = 1; /* 4-bit images are loaded as 8-bit ones (gDPLoadTextureTile_4b) */
    uint32_t sl = (uls & 0xFFF) >> 2, tl = (ult & 0xFFF) >> 2, sh = (lrs & 0xFFF) >> 2, th = (lrt & 0xFFF) >> 2;
    if (sh < sl || th < tl) return;
    Load l;
    memset(&l, 0, sizeof l);
    l.lo = (uint32_t)t->tmem * 8;
    l.row_tmem = (uint32_t)t->line * 8;
    /* the RDP writes whole 64-bit words per row at the tile's line: a row longer than the line runs
     * into the next, which then overwrites it */
    l.row_bytes = ((sh - sl + 1) * (uint32_t)bpp + 7) & ~7u;
    if (!l.row_tmem) l.row_tmem = l.row_bytes;
    l.rows = th - tl + 1;
    l.hi = l.lo + (l.rows - 1) * l.row_tmem + l.row_bytes;
    l.src = st.timg.p.p + ((size_t)tl * st.timg.width + sl) * (size_t)bpp;
    l.row_src = st.timg.width * (uint32_t)bpp;
    l.native = (uint8_t)st.timg.p.native;
    l.unit = (uint8_t)bpp;
    add_load(&l);
}

void nd_2d_load_block(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    (void)dxt;
    Tile *t = &st.tile[tile & 7];
    if (!st.timg.p.p) return;
    int bpp = bytes_per_texel(st.timg.siz);
    uint32_t texels = (lrs & 0xFFF) - (uls & 0xFFF) + 1;
    uint32_t bytes = bpp ? texels * (uint32_t)bpp : (texels + 1) / 2;
    Load l;
    memset(&l, 0, sizeof l);
    l.lo = (uint32_t)t->tmem * 8;
    l.hi = l.lo + ((bytes + 7) & ~7u);
    l.row_tmem = 0;
    l.row_bytes = bytes;
    size_t start = ((size_t)(ult & 0xFFF) * st.timg.width + (uls & 0xFFF));
    l.src = st.timg.p.p + (bpp ? start * (size_t)bpp : start / 2);
    l.native = (uint8_t)st.timg.p.native;
    l.unit = (uint8_t)(bpp > 1 ? bpp : 1);
    add_load(&l);
}

void nd_2d_load_tlut(uint32_t tile, uint32_t count) {
    Tile *t = &st.tile[tile & 7];
    if (!st.timg.p.p) return;
    int base = (int)t->tmem - 0x100;
    for (uint32_t k = 0; k <= (count & 0x3FF); k++) {
        int e = base + (int)k;
        if (e < 0 || e >= 256) continue;
        st.tlut_src[e] = st.timg.p.p + k * 2;
        st.tlut_native[e] = (uint8_t)st.timg.p.native;
    }
    st.tmem_gen++;
}

void nd_2d_texture(uint32_t s, uint32_t t, uint32_t level, uint32_t tile, uint32_t on) {
    st.tex_s = (uint16_t)s; st.tex_t = (uint16_t)t;
    st.tex_level = (uint8_t)(level & 7); st.tex_tile = (uint8_t)(tile & 7); st.tex_on = (uint8_t)(on & 1);
}

void nd_2d_geometry(uint32_t clear, uint32_t set) {
    st.geometry = (st.geometry & ~clear) | set;
}

/* ---- matrices ----------------------------------------------------------------------------------- */

static void mul(const float *a, const float *b, float *c) { /* c = a * b (row vectors) */
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = s;
        }
    memcpy(c, r, sizeof r);
}

void nd_2d_matrix(uintptr_t address, uint32_t flags) {
    Ptr p = resolve(address);
    if (!p.p) return;
    float m[16];
    if (p.native) {
        nd_decode_mtx(p.p, m);
    } else {
        uint32_t w[16];
        for (int i = 0; i < 16; i++) w[i] = be32(p.p + i * 4);
        nd_decode_mtx(w, m);
    }
    float *dst = (flags & G_MTX_PROJECTION) ? st.proj : st.mv;
    if (flags & G_MTX_LOAD) memcpy(dst, m, sizeof m);
    else mul(m, dst, dst);
}

/* ---- TMEM and textures ---------------------------------------------------------------------------- */

static uint8_t tmem_byte(uint32_t a) {
    a &= 0xFFF;
    for (int i = st.load_count - 1; i >= 0; i--) {
        const Load *l = &st.load[i];
        if (a < l->lo || a >= l->hi) continue;
        uint32_t off = a - l->lo;
        if (l->row_tmem) {
            uint32_t row = off / l->row_tmem, col = off % l->row_tmem;
            if (row >= l->rows) row = l->rows - 1, col = off - row * l->row_tmem;
            if (col >= l->row_bytes) continue;
            return mem_byte(l->src + (size_t)row * l->row_src + col, l->native, l->unit);
        }
        if (off >= l->row_bytes) continue;
        return mem_byte(l->src + off, l->native, l->unit);
    }
    return 0;
}

static uint32_t rgba8(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; }
static uint32_t c5(uint32_t v) { return v << 3 | v >> 2; }

/* A texel of a tile at (s, t) of its own (after mask or clamp): RGBA8, or a palette index (CI) */
static uint32_t texel(const Tile *t, uint32_t s, uint32_t tt, int ci) {
    uint32_t base = (uint32_t)t->tmem * 8 + tt * (uint32_t)t->line * 8;
    switch (t->siz) {
    case 0: {
        uint32_t b = tmem_byte(base + s / 2);
        uint32_t v = (s & 1) ? b & 0xF : b >> 4;
        if (ci) return v;
        if (t->fmt == 3) { uint32_t i = v >> 1; i = i << 5 | i << 2 | i >> 1; return rgba8(i, i, i, (v & 1) ? 255 : 0); }
        v = v << 4 | v;
        return rgba8(v, v, v, v);
    }
    case 1: {
        uint32_t v = tmem_byte(base + s);
        if (ci) return v;
        if (t->fmt == 3) { uint32_t i = (v >> 4) * 17, a = (v & 15) * 17; return rgba8(i, i, i, a); }
        return rgba8(v, v, v, v);
    }
    case 2: {
        uint32_t v = (uint32_t)tmem_byte(base + s * 2) << 8 | tmem_byte(base + s * 2 + 1);
        if (ci) return v & 0xFF;
        if (t->fmt == 3) return rgba8(v >> 8, v >> 8, v >> 8, v & 0xFF);
        return rgba8(c5(v >> 11 & 31), c5(v >> 6 & 31), c5(v >> 1 & 31), (v & 1) ? 255 : 0);
    }
    default: {
        uint32_t a = base + s * 2;
        return rgba8(tmem_byte(a), tmem_byte(a + 1), tmem_byte(a + 0x800), tmem_byte(a + 0x801));
    }
    }
}

static uint32_t *scratch;
static uint32_t scratch_cap;

/* the palette of a CI tile: its entries' memory (read each frame), or their values now */
static uint16_t tile_palette(int first, int count) {
    const uint8_t *src = st.tlut_src[first];
    int native = st.tlut_native[first], run = src != NULL;
    for (int k = 1; k < count && run; k++)
        run = st.tlut_src[first + k] == src + 2 * k && st.tlut_native[first + k] == native;
    uint32_t id;
    if (run) {
        id = nd_model_palette_source(src, native, count);
    } else {
        uint16_t v[256];
        for (int k = 0; k < count; k++) {
            const uint8_t *p = st.tlut_src[first + k];
            v[k] = p ? (uint16_t)(mem_byte(p, st.tlut_native[first + k], 2) << 8 | mem_byte(p + 1, st.tlut_native[first + k], 2))
                     : 0;
        }
        id = nd_model_palette_values(v, count);
    }
    return id == ~0u ? 0 : (uint16_t)id;
}

static int full; /* the renderer's buffers are full: reset at the end of this item */

/* A tile decoded over what it can address (as nd_model.c's decode_tile) */
static void decode_tile(int index, ElRdTile *o) {
    const Tile *t = &st.tile[index & 7];
    memset(o, 0, sizeof *o);
    int tlut = (st.othermode_h >> 14 & 3) != 0;
    int ci = t->fmt == 2 || (tlut && t->siz <= 1);
    uint32_t cw = (uint32_t)((t->sh >> 2) - (t->sl >> 2) + 1), ch = (uint32_t)((t->th >> 2) - (t->tl >> 2) + 1);
    uint32_t w = t->masks ? 1u << t->masks : cw, h = t->maskt ? 1u << t->maskt : ch;
    if (w < 1 || w > 1024 || h < 1 || h > 1024) {
        LOG2D("nd_2d: tile %d of %ux%u skipped\n", index, w, h);
        return;
    }
    if (w * h > scratch_cap) {
        scratch_cap = w * h;
        scratch = realloc(scratch, scratch_cap * 4);
    }
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) scratch[y * w + x] = texel(t, x, y, ci);
    uint32_t off = nd_model_texture(scratch, w * h);
    if (off == ~0u) { full = 1; return; }
    o->texels = off;
    o->width = (uint16_t)w;
    o->height = (uint16_t)h;
    o->format = ci ? EL_RDT_CI : EL_RDT_RGBA;
    o->cms = (uint8_t)(t->cms | (t->masks ? 0 : 2));
    o->cmt = (uint8_t)(t->cmt | (t->maskt ? 0 : 2));
    o->masks = t->masks; o->maskt = t->maskt; o->shifts = t->shifts; o->shiftt = t->shiftt;
    o->sl = (int16_t)t->sl; o->tl = (int16_t)t->tl; o->sh = (int16_t)t->sh; o->th = (int16_t)t->th;
    if (ci) o->palette = tile_palette(t->siz == 0 ? t->palette * 16 : 0, t->siz == 0 ? 16 : 256);
}

/* decoded tiles, until TMEM, the tile or the TLUT mode change */
static struct { int valid; Tile tile; uint32_t gen, tlut; ElRdTile out; } tile_cache[8];

static void tile_for(int index, ElRdTile *o) {
    int i = index & 7;
    uint32_t tlut = st.othermode_h >> 14 & 3;
    if (tile_cache[i].valid && tile_cache[i].gen == st.tmem_gen && tile_cache[i].tlut == tlut &&
        !memcmp(&tile_cache[i].tile, &st.tile[i], sizeof(Tile))) {
        *o = tile_cache[i].out;
        if (o->format == EL_RDT_CI) {
            /* mark the palette used this frame */
            const Tile *t = &st.tile[i];
            o->palette = tile_palette(t->siz == 0 ? t->palette * 16 : 0, t->siz == 0 ? 16 : 256);
        }
        return;
    }
    decode_tile(i, o);
    if (full) return;
    tile_cache[i].valid = 1;
    tile_cache[i].tile = st.tile[i];
    tile_cache[i].gen = st.tmem_gen;
    tile_cache[i].tlut = tlut;
    tile_cache[i].out = *o;
}

/* Which texels the combiner reads: bit 0 TEXEL0, bit 1 TEXEL1 (the second cycle swaps them) */
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

/* ---- materials ------------------------------------------------------------------------------------ */

typedef struct MatEntry { ElRdMaterial m; uint32_t id; struct MatEntry *next; } MatEntry;
static MatEntry *mat_table[1024];

static uint32_t fnv32(const void *p, size_t n) {
    const uint8_t *b = p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

static uint32_t material_id(const ElRdMaterial *m) {
    uint32_t h = fnv32(m, sizeof *m) & 1023;
    for (MatEntry *e = mat_table[h]; e; e = e->next)
        if (!memcmp(&e->m, m, sizeof *m)) return e->id;
    uint32_t id = nd_api()->material_2d(m);
    if (id == ~0u) { full = 1; return ~0u; }
    MatEntry *e = malloc(sizeof *e);
    e->m = *m;
    e->id = id;
    e->next = mat_table[h];
    mat_table[h] = e;
    return id;
}

/* The material of a rectangle: the state now, the tile `tile` (and the next) sampled if the combiner
 * reads them */
static void make_material(ElRdMaterial *m, int tile, int textured) {
    memset(m, 0, sizeof *m);
    m->combine[0] = st.combine[0] & 0xFFFFFF;
    m->combine[1] = st.combine[1];
    m->othermode_h = st.othermode_h;
    m->othermode_l = st.othermode_l;
    m->prim_lod_frac = st.prim_lod;
    m->tile_index = (uint8_t)(tile & 7);
    if (!textured) return;
    int used = combiner_texels();
    if (used) m->flags |= EL_RDM_TEXTURE;
    if (used & 1) tile_for(tile, &m->tile[0]);
    if (used & 2) tile_for(tile + 1, &m->tile[1]);
}

/* ---- items ---------------------------------------------------------------------------------------- */

void nd_2d_reset(void) {
    for (int i = 0; i < 1024; i++)
        while (mat_table[i]) { MatEntry *e = mat_table[i]; mat_table[i] = e->next; free(e); }
    memset(tile_cache, 0, sizeof tile_cache);
}

static void emit(ElRd2D *d) {
    d->id = (uint64_t)(nd2d_kind & 0xFF) << 56 | (uint64_t)(element & 0xFFFF) << 40 | item_number++;
    if (!(d->flags & EL_RD2D_ANCHOR_MASK)) d->flags |= (uint32_t)anchor; /* a rectangle's own (a full-width fill) first */
    memcpy(d->env, st.env, 4);
    memcpy(d->prim, st.prim, 4);
    memcpy(d->blend, st.blend, 4);
    memcpy(d->fog, st.fog, 4);
    nd_api()->draw_2d(d);
}

/* a rectangle with the current state; `fill` (FILL mode): the colour is the prim register (set to the
 * fill colour by the caller) */
static void rect_item(float x0, float y0, float x1, float y1, int tile, float s, float t, float dsdx, float dtdy, int textured,
                      int fill) {
    if (x1 <= x0 || y1 <= y0) return;
    for (int attempt = 0; attempt < 2; attempt++) {
        ElRdMaterial m;
        full = 0;
        if (fill) {
            memset(&m, 0, sizeof m);
            /* 1-cycle, colour and alpha = PRIM (the fill colour), written as it is */
            m.combine[0] = 15u << 20 | 31u << 15 | 7u << 12 | 7u << 9 | 15u << 5 | 31u;
            m.combine[1] = 15u << 28 | 15u << 24 | 7u << 21 | 7u << 18 | 3u << 15 | 7u << 12 | 3u << 9 | 3u << 6 | 7u << 3 | 3u;
            m.othermode_h = st.othermode_h & ~(3u << 20);
            m.othermode_l = 0;
        } else {
            make_material(&m, tile, textured);
        }
        uint32_t id = full ? ~0u : material_id(&m);
        if (log_on > 1)
            fprintf(stderr, "nd_2d: rect %g,%g-%g,%g st %g,%g cc %06x:%08x om %06x:%08x blend %02x%02x%02x%02x env %02x%02x%02x%02x "
                    "tile %d fmt %d %ux%u pal %u\n", x0, y0, x1, y1, s, t, m.combine[0], m.combine[1], m.othermode_h,
                    m.othermode_l, st.blend[0], st.blend[1], st.blend[2], st.blend[3], st.env[0], st.env[1], st.env[2], st.env[3],
                    tile, m.tile[0].format, m.tile[0].width, m.tile[0].height, m.tile[0].palette);
        if (full || id == ~0u) {
            fprintf(stderr, "nd_2d: the renderer's buffers are full: loading everything again\n");
            nd_model_reset();
            continue;
        }
        ElRd2D d;
        memset(&d, 0, sizeof d);
        d.mesh = EL_RD2D_RECT;
        d.material = id;
        d.rect[0] = x0; d.rect[1] = y0; d.rect[2] = x1; d.rect[3] = y1;
        d.st[0] = s; d.st[1] = t; d.st[2] = dsdx; d.st[3] = dtdy;
        if (fill || !textured) {
            d.flags = EL_RD2D_FILL;
            if (x0 <= 8 && x1 >= 312) d.flags |= EL_RD2D_ANCHOR_STRETCH; /* covers the screen's width */
        }
        emit(&d);
        return;
    }
}

void nd_2d_texrect(int32_t xl, int32_t yl, int32_t xh, int32_t yh, uint32_t tile, int32_t s, int32_t t, int32_t dsdx,
                   int32_t dtdy) {
    if (!recording) return;
    xl &= 0xFFF; yl &= 0xFFF; xh &= 0xFFF; yh &= 0xFFF;
    if (ND_WIDE() && anchor == EL_RD2D_ANCHOR_WORLD) {
        /* R7: on a wider picture a marker or a number placed from the world can be left of the 4:3 screen
         * (x < 0, which the RDP's 12 bits would wrap): the coordinates as signed 10.2 */
        xl = (xl ^ 0x800) - 0x800; xh = (xh ^ 0x800) - 0x800;
    }
    rect_item(xl / 4.0f, yl / 4.0f, xh / 4.0f, yh / 4.0f, (int)(tile & 7), (int16_t)s / 32.0f, (int16_t)t / 32.0f,
              (int16_t)dsdx / 1024.0f, (int16_t)dtdy / 1024.0f, 1, 0);
}

void nd_2d_fillrect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    if (!recording) return;
    ulx &= 0x3FF; uly &= 0x3FF; lrx &= 0x3FF; lry &= 0x3FF;
    int cycle = (int)(st.othermode_h >> 20 & 3);
    if (cycle == 3) { /* FILL: the corners are included */
        uint8_t save[4];
        memcpy(save, st.prim, 4);
        uint32_t c = st.fill >> 16;
        st.prim[0] = (uint8_t)c5(c >> 11 & 31);
        st.prim[1] = (uint8_t)c5(c >> 6 & 31);
        st.prim[2] = (uint8_t)c5(c >> 1 & 31);
        st.prim[3] = (c & 1) ? 255 : 0;
        rect_item((float)ulx, (float)uly, (float)lrx + 1, (float)lry + 1, 0, 0, 0, 0, 0, 0, 1);
        memcpy(st.prim, save, 4);
        return;
    }
    rect_item((float)ulx, (float)uly, (float)lrx, (float)lry, 0, 0, 0, 0, 0, 0, 0);
}

/* ---- static lists ----------------------------------------------------------------------------------- */

enum {
    OP_OTHERMODE, OP_COLOR, OP_PRIM, OP_COMBINE, OP_TIMG, OP_TILE, OP_TILESIZE, OP_LOADTILE, OP_LOADBLOCK, OP_LOADTLUT,
    OP_TEXTURE, OP_GEOMETRY, OP_TEXRECT, OP_FILLRECT, OP_LIST_START, OP_MESH,
};

typedef struct {
    uint8_t op;
    uint32_t a[12];
    uintptr_t p;
} Op;

typedef struct List {
    uintptr_t address;
    Op *ops;
    int count, cap;
    uintptr_t lo, hi; /* the memory it was read from (a DMA over it drops it) */
    struct List *next;
} List;

static List *list_table[256];

static Op *add_op(List *l, int op) {
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->ops = realloc(l->ops, (size_t)l->cap * sizeof(Op));
    }
    Op *o = &l->ops[l->count++];
    memset(o, 0, sizeof *o);
    o->op = (uint8_t)op;
    return o;
}

static void note(List *l, const uint8_t *p, size_t n) {
    uintptr_t lo = (uintptr_t)p, hi = lo + n;
    if (!l->hi) { l->lo = lo; l->hi = hi; return; }
    if (lo < l->lo) l->lo = lo;
    if (hi > l->hi) l->hi = hi;
}

/* Read a static list (and the lists it calls) into calls */
static void compile(List *l, uintptr_t address, int depth) {
    if (depth > 8) return;
    Ptr p = resolve(address);
    if (!p.p) return;
    Op *start = add_op(l, OP_LIST_START);
    start->p = address;
    int start_index = l->count - 1, mesh = 0;
    for (int i = 0; i < 4096; i++) {
        const uint8_t *c = p.p + i * 8;
        uint32_t w0, w1;
        if (p.native) { memcpy(&w0, c, 4); memcpy(&w1, c + 4, 4); }
        else { w0 = be32(c); w1 = be32(c + 4); }
        note(l, c, 8);
        Op *o;
        switch (w0 >> 24) {
        case 0x00: case 0xE6: case 0xE7: case 0xE8: case 0xE9: case 0xBE: case 0xBC: break;
        case 0xB8: goto done;
        case 0x06:
            compile(l, w1, depth + 1);
            if ((w0 >> 16 & 0xFF) == 1) goto done;
            break;
        case 0xBA: case 0xB9:
            o = add_op(l, OP_OTHERMODE);
            o->a[0] = w0 >> 24; o->a[1] = w0 >> 8 & 0xFF; o->a[2] = w0 & 0xFF; o->a[3] = w1;
            break;
        case 0xEF:
            o = add_op(l, OP_OTHERMODE);
            o->a[0] = G_SETOTHERMODE_H; o->a[1] = 0; o->a[2] = 24; o->a[3] = w0 & 0xFFFFFF;
            o = add_op(l, OP_OTHERMODE);
            o->a[0] = G_SETOTHERMODE_L; o->a[1] = 0; o->a[2] = 32; o->a[3] = w1;
            break;
        case 0xFC: o = add_op(l, OP_COMBINE); o->a[0] = w0 & 0xFFFFFF; o->a[1] = w1; break;
        case 0xFB: case 0xF9: case 0xF8: case 0xF7: o = add_op(l, OP_COLOR); o->a[0] = w0 >> 24; o->a[1] = w1; break;
        case 0xFA: o = add_op(l, OP_PRIM); o->a[0] = w0 >> 8 & 0xFF; o->a[1] = w0 & 0xFF; o->a[2] = w1; break;
        case 0xFD:
            o = add_op(l, OP_TIMG);
            o->a[0] = w0 >> 21 & 7; o->a[1] = w0 >> 19 & 3; o->a[2] = (w0 & 0xFFF) + 1; o->p = w1;
            break;
        case 0xF5:
            o = add_op(l, OP_TILE);
            o->a[0] = w0 >> 21 & 7; o->a[1] = w0 >> 19 & 3; o->a[2] = w0 >> 9 & 0x1FF; o->a[3] = w0 & 0x1FF;
            o->a[4] = w1 >> 24 & 7; o->a[5] = w1 >> 20 & 15; o->a[6] = w1 >> 18 & 3; o->a[7] = w1 >> 14 & 15;
            o->a[8] = w1 >> 10 & 15; o->a[9] = w1 >> 8 & 3; o->a[10] = w1 >> 4 & 15; o->a[11] = w1 & 15;
            break;
        case 0xF2: case 0xF4: case 0xF3:
            o = add_op(l, (w0 >> 24) == 0xF2 ? OP_TILESIZE : (w0 >> 24) == 0xF4 ? OP_LOADTILE : OP_LOADBLOCK);
            o->a[0] = w1 >> 24 & 7; o->a[1] = w0 >> 12 & 0xFFF; o->a[2] = w0 & 0xFFF; o->a[3] = w1 >> 12 & 0xFFF;
            o->a[4] = w1 & 0xFFF;
            break;
        case 0xF0: o = add_op(l, OP_LOADTLUT); o->a[0] = w1 >> 24 & 7; o->a[1] = w1 >> 14 & 0x3FF; break;
        case 0xBB:
            o = add_op(l, OP_TEXTURE);
            o->a[0] = w1 >> 16; o->a[1] = w1 & 0xFFFF; o->a[2] = w0 >> 11 & 7; o->a[3] = w0 >> 8 & 7; o->a[4] = w0 & 0xFF;
            break;
        case 0xB6: o = add_op(l, OP_GEOMETRY); o->a[0] = w1; break;
        case 0xB7: o = add_op(l, OP_GEOMETRY); o->a[1] = w1; break;
        case 0xE4: case 0xE5: {
            uint32_t h1 = 0, h2 = 0;
            for (int k = 1; k <= 2; k++) {
                const uint8_t *n = c + k * 8;
                uint32_t x0, x1;
                if (p.native) { memcpy(&x0, n, 4); memcpy(&x1, n + 4, 4); }
                else { x0 = be32(n); x1 = be32(n + 4); }
                if (k == 1) h1 = x1; else h2 = x1;
                (void)x0;
            }
            note(l, c, 24);
            i += 2;
            o = add_op(l, OP_TEXRECT);
            o->a[0] = w1 >> 12 & 0xFFF; o->a[1] = w1 & 0xFFF; o->a[2] = w0 >> 12 & 0xFFF; o->a[3] = w0 & 0xFFF;
            o->a[4] = w1 >> 24 & 7; o->a[5] = h1 >> 16; o->a[6] = h1 & 0xFFFF; o->a[7] = h2 >> 16; o->a[8] = h2 & 0xFFFF;
            break; }
        case 0xF6:
            o = add_op(l, OP_FILLRECT);
            o->a[0] = w1 >> 14 & 0x3FF; o->a[1] = w1 >> 2 & 0x3FF; o->a[2] = w0 >> 14 & 0x3FF; o->a[3] = w0 >> 2 & 0x3FF;
            break;
        case 0x04: case 0xBF: case 0xB1:
            if (!mesh) {
                mesh = 1;
                l->ops[start_index].a[0] = 1; /* this list's start state enters the mesh */
                o = add_op(l, OP_MESH);
                o->p = address;
            }
            break;
        case 0xB4: case 0xB3: case 0xB2: break;
        default:
            LOG2D("nd_2d: op %02x (%08x %08x) in the static list %08x (ignored)\n", w0 >> 24, w0, w1, (uint32_t)address);
            break;
        }
    }
done:;
}

/* The entry state a mesh is made with: the 2D state at its list's call, as a native list (interned) */
typedef struct Entry { uint32_t hash; int count; Gfx *gfx; struct Entry *next; } Entry;
static Entry *entry_table[64];

typedef struct {
    uint32_t othermode_h, othermode_l, combine[2], geometry;
    uint16_t tex_s, tex_t;
    uint8_t tex_level, tex_tile, tex_on;
    const uint8_t *tlut_src[256];
} Snapshot;

static const Gfx *entry_for(const Snapshot *s) {
    Gfx g[64];
    int n = 0;
#define W(a, b) (g[n].words.w0 = (a), g[n].words.w1 = (b), n++)
    W(0xBAu << 24 | 0 << 8 | 24, s->othermode_h);
    W(0xB9u << 24 | 0 << 8 | 32, s->othermode_l);
    W(0xFCu << 24 | s->combine[0], s->combine[1]);
    W(0xB6u << 24, 0xFFFFFFFFu);
    W(0xB7u << 24, s->geometry);
    W(0xBBu << 24 | (uint32_t)s->tex_level << 11 | (uint32_t)s->tex_tile << 8 | s->tex_on, (uint32_t)s->tex_s << 16 | s->tex_t);
    /* the TLUT, in runs */
    for (int e = 0; e < 256 && n < 56;) {
        if (!s->tlut_src[e]) { e++; continue; }
        int k = 1;
        while (e + k < 256 && s->tlut_src[e + k] == s->tlut_src[e] + 2 * k) k++;
        W(0xFDu << 24 | 0u << 21 | 2u << 19, (uint32_t)(uintptr_t)s->tlut_src[e]);
        W(0xF5u << 24 | (uint32_t)(0x100 + e), 7u << 24);
        W(0xF0u << 24, 7u << 24 | (uint32_t)((k - 1) << 2) << 12);
        e += k;
    }
    W(0xB8u << 24, 0);
#undef W
    uint32_t h = fnv32(g, (size_t)n * sizeof(Gfx));
    for (Entry *e = entry_table[h & 63]; e; e = e->next)
        if (e->hash == h && e->count == n && !memcmp(e->gfx, g, (size_t)n * sizeof(Gfx))) return e->gfx;
    /* the model loader takes list addresses as 32 bits (the image's .bss is below 4 GiB, the heap need not be) */
    static Gfx pool[64 * 64];
    static int pool_used;
    if (pool_used + n > 64 * 64) {
        LOG2D("nd_2d: no room for another mesh entry state\n");
        return NULL;
    }
    Entry *e = malloc(sizeof *e);
    e->hash = h;
    e->count = n;
    e->gfx = &pool[pool_used];
    pool_used += n;
    memcpy(e->gfx, g, (size_t)n * sizeof(Gfx));
    e->next = entry_table[h & 63];
    entry_table[h & 63] = e;
    return e->gfx;
}

static void mesh_item(uintptr_t address, const Snapshot *s) {
    const Gfx *entry = entry_for(s);
    if (!entry) return;
    const NdMesh *mesh = nd_model_get((uint32_t)address, entry);
    if (!mesh) return;
    nd_model_use(mesh);
    ElRd2D d;
    memset(&d, 0, sizeof d);
    d.mesh = nd_model_id(mesh);
    mul(st.mv, st.proj, d.matrix);
    memcpy(d.model, st.mv, sizeof d.model);
    /* where its origin lands on the screen (R7's anchor) */
    float w = d.matrix[15] != 0 ? d.matrix[15] : 1;
    float x = (d.matrix[12] / w + 1) * 160, y = (1 - d.matrix[13] / w) * 120;
    d.rect[0] = d.rect[2] = x;
    d.rect[1] = d.rect[3] = y;
    emit(&d);
}

static void replay(const List *l) {
    Snapshot snap[9];
    int depth = -1;
    for (int i = 0; i < l->count; i++) {
        const Op *o = &l->ops[i];
        const uint32_t *a = o->a;
        switch (o->op) {
        case OP_OTHERMODE: nd_2d_othermode(a[0], a[1], a[2], a[3]); break;
        case OP_COLOR: nd_2d_color(a[0], a[1]); break;
        case OP_PRIM: nd_2d_prim_color(a[0], a[1], a[2]); break;
        case OP_COMBINE: nd_2d_combine(a[0], a[1]); break;
        case OP_TIMG: nd_2d_texture_image(a[0], a[1], a[2], o->p); break;
        case OP_TILE: nd_2d_tile(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]); break;
        case OP_TILESIZE: nd_2d_tile_size(a[0], a[1], a[2], a[3], a[4]); break;
        case OP_LOADTILE: nd_2d_load_tile(a[0], a[1], a[2], a[3], a[4]); break;
        case OP_LOADBLOCK: nd_2d_load_block(a[0], a[1], a[2], a[3], a[4]); break;
        case OP_LOADTLUT: nd_2d_load_tlut(a[0], a[1]); break;
        case OP_TEXTURE: nd_2d_texture(a[0], a[1], a[2], a[3], a[4]); break;
        case OP_GEOMETRY: nd_2d_geometry(a[0], a[1]); break;
        case OP_TEXRECT: nd_2d_texrect((int32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], a[4], (int32_t)a[5], (int32_t)a[6],
                                       (int32_t)a[7], (int32_t)a[8]); break;
        case OP_FILLRECT: nd_2d_fillrect((int32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3]); break;
        case OP_LIST_START:
            if (depth < 8) depth++;
            if (a[0]) {
                Snapshot *s = &snap[depth];
                s->othermode_h = st.othermode_h; s->othermode_l = st.othermode_l;
                s->combine[0] = st.combine[0]; s->combine[1] = st.combine[1];
                s->geometry = st.geometry;
                s->tex_s = st.tex_s; s->tex_t = st.tex_t;
                s->tex_level = st.tex_level; s->tex_tile = st.tex_tile; s->tex_on = st.tex_on;
                memcpy(s->tlut_src, st.tlut_src, sizeof s->tlut_src);
            }
            break;
        case OP_MESH:
            if (recording && depth >= 0) mesh_item(o->p, &snap[depth]);
            break;
        }
    }
}

void nd_2d_display_list(uintptr_t address) {
    uint32_t h = (uint32_t)(address * 2654435761u) >> 24;
    List *l;
    for (l = list_table[h]; l; l = l->next)
        if (l->address == address) break;
    if (!l) {
        l = calloc(1, sizeof *l);
        l->address = address;
        compile(l, address, 0);
        l->next = list_table[h];
        list_table[h] = l;
        LOG2D("nd_2d: static list %08x: %d calls\n", (uint32_t)address, l->count);
    }
    replay(l);
}

/* A ROM DMA wrote [host, host + bytes): the lists read from there are read again */
void nd_2d_forget(uintptr_t host, uint32_t bytes) {
    uintptr_t lo = host, hi = host + bytes;
    for (int i = 0; i < 256; i++)
        for (List **pl = &list_table[i]; *pl;) {
            List *l = *pl;
            if (l->lo < hi && lo < l->hi) { *pl = l->next; free(l->ops); free(l); }
            else pl = &l->next;
        }
}

/* gDPLoadTextureBlock's commands, for code that writes them itself: the macro into a scratch list, so the
 * core gets exactly its calls */
void nd_2d_load_texture_block(uintptr_t timg, uint32_t fmt, uint32_t siz, uint32_t width, uint32_t height, uint32_t pal,
                              uint32_t cms, uint32_t cmt, uint32_t masks, uint32_t maskt, uint32_t shifts, uint32_t shiftt) {
    Gfx scratch_list[16];
    Gfx *g = scratch_list;
    switch (siz) {
    case G_IM_SIZ_4b:
        gDPLoadTextureBlock_4b(g++, timg, fmt, width, height, pal, cms, cmt, masks, maskt, shifts, shiftt);
        break;
    case G_IM_SIZ_8b:
        gDPLoadTextureBlock(g++, timg, fmt, G_IM_SIZ_8b, width, height, pal, cms, cmt, masks, maskt, shifts, shiftt);
        break;
    case G_IM_SIZ_16b:
        gDPLoadTextureBlock(g++, timg, fmt, G_IM_SIZ_16b, width, height, pal, cms, cmt, masks, maskt, shifts, shiftt);
        break;
    default:
        gDPLoadTextureBlock(g++, timg, fmt, G_IM_SIZ_32b, width, height, pal, cms, cmt, masks, maskt, shifts, shiftt);
        break;
    }
}

/* One command the code wrote as words itself (GFX_WORDS in the overlays: othermodes, combiner, colours,
 * a display list call), the way a static list's commands are taken */
void nd_2d_words(uint32_t w0, uint32_t w1) {
    Gfx g[2];
    g[0].words.w0 = w0;
    g[0].words.w1 = w1;
    g[1].words.w0 = (uint32_t)G_ENDDL << 24;
    g[1].words.w1 = 0;
    List l;
    memset(&l, 0, sizeof l);
    compile(&l, (uintptr_t)g, 0);
    replay(&l);
    free(l.ops);
}
