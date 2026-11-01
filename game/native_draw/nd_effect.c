/* nd_effect.c: effects (EL_KIND_EFFECT) and the screen quads (EL_KIND_SCREEN) drawn natively
 * (docs/renderer/NATIVE_DRAW.md, "Effects"). PC-only.
 *
 * What draws what (Gfx_BuildFrameDisplayList, after the shadows):
 * - Effect_UpdateAndDrawSprites: the 256 sprite slots (foot dust, map-object puffs, arcs and drops,
 *   spirit sparkles, flipbook puffs, converge and burst particles). Per sprite: its texture
 *   (Effect_LoadSpriteTexture, when it differs from the last sprite's), its update callback (moves it,
 *   queues the env colour) or the default setup list, a matrix aimed at the eye (with a roll) or aligned
 *   to its vector, and its list. The sprite lists are self-contained (cycle type, render mode, geometry,
 *   combiner, TEXTURE, a TLUT for CI ones) but take the texture and TLUT mode from the top level.
 * - Effect_UpdateAndDrawAll: streaks (D_800EB880 once, D_800EB8E8 each), stat-up markers (D_800EB990,
 *   D_800EB9C8), terrain rings (D_800C4780 + an 8x8 I8 texture once, 16 segments D_800C4840 each),
 *   model effects (D_800EB6D0 once; per effect Effect_ConfigureModelEffectTexture loads an I8 or CI8
 *   texture with its TLUT, sets a combiner, the scroll as the tile size, TEXTURE and lighting/texgen,
 *   then the env colour and culling, then Anim_DrawNode walks the effect's AnimSet: its node lists are
 *   geometry only), then the floating numbers (2D texture rectangles: not here, EL_KIND_HUD's).
 * - Fade_UpdateAndDraw (EL_KIND_SCREEN): the letterbox bars (two fill rectangles) and the fade quad
 *   (the ortho projection, an identity modelview, the ramp's env colour, one of two render modes).
 *
 * So most of an effect's state is queued at the top level. Each draw here gets an entry list made of
 * the same commands (the game's own macros, Effect_LoadSpriteTexture itself for sprites) after the state
 * the effect section starts with: 2-cycle (the frame's; every effect setup list sets 2-cycle again),
 * the map's setup list D_10000E8 (texture perspective, filter, LOD, conversion bits as the shadows'
 * D_1000270 leaves them), bilinear, and the TLUT mode the last sprite texture left. Entry lists are
 * interned by content (a static pool: lists must stay below 4 GiB and live as long as their meshes);
 * a mesh is (list, entry list). Colours are the draw's: the game sites call nd_env_color where they
 * queue gDPSetEnvColor (the flicker of odd and even ticks is that tick's value).
 *
 * Ids (R6): kind << 56 | pass << 48 | sub << 40 | slot << 32 | birth tick (the tick the slot's effect
 * started: sprites and streaks by their life counting down, rings at their first growing frame, model
 * effects by their age, markers at their start). Flipbook frames (the sprite's texture, the model
 * effect's image) are a different entry list, so a different mesh: R6 snaps them.
 *
 * ELTALE_ND_EFFECT_LOG=1 prints per tick (stdout, before the tick's P1_GFX_TASK line) how many of each
 * were drawn natively. */
#include "game/native_draw/nd_internal.h"
#include "game/native_draw/nd_effect.h"

#include "game/core/main.h"
#include "game/camera/camera.h"
#include "game/gfx/effect.h"
#include "game/gfx/fade.h"
#include "game/gfx/sprite_effect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern Gfx D_10000E8[];  /* segment 1: the map's setup list */
extern Gfx D_800EB6D0[]; /* the model effects' setup */
extern Gfx D_800EB880[]; /* the streaks' setup */
extern Gfx D_800EB8E8[]; /* a streak */
extern Gfx D_800EB990[]; /* the stat-up markers' setup */
extern Gfx D_800EB9C8[]; /* a marker */
extern Gfx D_800C4780[]; /* the rings' setup */
extern u8 D_800C47C0[];  /* the rings' 8x8 I8 texture */
extern Gfx D_800C4840[]; /* a ring segment */

enum { PASS_SPRITE = 0, PASS_STREAK = 1, PASS_MARKER = 2, PASS_RING = 3, PASS_MODEL = 4 };
enum { PASS_FADE = 0, PASS_LETTERBOX = 1 };

static uint32_t tick;        /* effect ticks (nd_effect_begin) */
static int tlut_rgba16;      /* the TLUT mode the last sprite texture load left (CI sprites: RGBA16) */
static int log_on = -1;
static int counts[5], screen_counts[2];

/* ---- entry lists, interned by content ------------------------------------------------------------- */

enum { ENTRY_MAX = 1024, ENTRY_CMDS = 40, ENTRY_HASH = 4096 };
static Gfx entry_pool[ENTRY_MAX][ENTRY_CMDS];
static uint64_t entry_key[ENTRY_MAX];
static int entry_count;
static int16_t entry_table[ENTRY_HASH]; /* index + 1, open addressing */

static uint64_t fnv(const void *p, size_t n) {
    const uint8_t *b = p;
    uint64_t h = UINT64_C(0xCBF29CE484222325);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * UINT64_C(0x100000001B3);
    return h;
}

/* The pool's copy of `list` (n commands, then an end), or NULL when the pool is full */
static const void *intern(const Gfx *list, int n) {
    if (n >= ENTRY_CMDS) {
        fprintf(stderr, "native draw: effect entry list of %d commands (more than %d)\n", n, ENTRY_CMDS - 1);
        return NULL;
    }
    uint64_t key = fnv(list, (size_t)n * sizeof(Gfx)) ^ (uint64_t)n;
    for (uint32_t h = (uint32_t)key & (ENTRY_HASH - 1);; h = (h + 1) & (ENTRY_HASH - 1)) {
        int i = entry_table[h] - 1;
        if (i < 0) {
            if (entry_count == ENTRY_MAX) {
                static int said;
                if (!said++) fprintf(stderr, "native draw: effect entry lists: more than %d\n", ENTRY_MAX);
                return NULL;
            }
            i = entry_count++;
            memcpy(entry_pool[i], list, (size_t)n * sizeof(Gfx));
            gSPEndDisplayList(&entry_pool[i][n]);
            entry_key[i] = key;
            entry_table[h] = (int16_t)(i + 1);
            return entry_pool[i];
        }
        if (entry_key[i] == key && !memcmp(entry_pool[i], list, (size_t)n * sizeof(Gfx))) return entry_pool[i];
    }
}

/* What the frame list has set when the effect section starts (see the top) */
static Gfx *base(Gfx *g) {
    gDPSetCycleType(g++, G_CYC_2CYCLE);
    gSPDisplayList(g++, D_10000E8);
    gDPSetTextureFilter(g++, G_TF_BILERP);
    gDPSetTextureLUT(g++, tlut_rgba16 ? G_TT_RGBA16 : G_TT_NONE);
    return g;
}

/* ---- drawing ---------------------------------------------------------------------------------------- */

static uint64_t make_id(int kind, int pass, int sub, int slot, uint32_t birth) {
    return (uint64_t)kind << EL_RDD_KIND_SHIFT | (uint64_t)(pass & 0xFF) << 48 | (uint64_t)(sub & 0xFF) << 40 |
           (uint64_t)(slot & 0xFF) << 32 | birth;
}

/* Draw `dl` entered with `entry`; the caller fills the inputs (xform, pos, ...) of `d` */
static void draw(ElRdDraw *d, uint32_t dl, const void *entry, const void *mtx) {
    if (!entry) return;
    const NdMesh *mesh = nd_model_get(dl, entry);
    if (!mesh) return;
    nd_model_use(mesh);
    d->mesh = nd_model_id(mesh);
    if (mtx) nd_decode_mtx(mtx, d->matrix);
    nd_api()->draw(d);
}

void nd_effect_begin(void) {
    if (log_on < 0) log_on = getenv("ELTALE_ND_EFFECT_LOG") ? atoi(getenv("ELTALE_ND_EFFECT_LOG")) : 0;
    if (log_on && (counts[0] | counts[1] | counts[2] | counts[3] | counts[4] | screen_counts[0] | screen_counts[1]))
        printf("ND_EFFECT tick=%u sprites=%d streaks=%d markers=%d rings=%d model_nodes=%d fade=%d letterbox=%d\n", tick,
               counts[0], counts[1], counts[2], counts[3], counts[4], screen_counts[0], screen_counts[1]);
    memset(counts, 0, sizeof counts);
    memset(screen_counts, 0, sizeof screen_counts);
    tick++;
}

/* ---- sprites ---------------------------------------------------------------------------------------- */

static struct { uint32_t seen, birth; uint16_t life; } sprite_track[0x100];

void nd_effect_sprite(int slot, const EffectSprite *s, const void *mtx) {
    /* the entry: the effect section's state, the default setup list for sprites without an update
     * callback, then the game's own texture load (into a scratch list) */
    Gfx buf[ENTRY_CMDS], *g = base(buf);
    if (!s->update) {
        gSPDisplayList(g++, effect_sprite_setup_dl);
    }
    Gfx *save = gfx_dl_head;
    gfx_dl_head = g;
    Effect_LoadSpriteTexture(s->texFmt, s->tex, s->texWidth, s->texHeight, s->cms, s->cmt);
    g = gfx_dl_head;
    gfx_dl_head = save;
    tlut_rgba16 = s->texFmt >= 6;

    /* the slot's effect started when its life did not count down from the last tick's */
    if (sprite_track[slot].seen != tick - 1 || (uint16_t)(sprite_track[slot].life - 1) != s->life)
        sprite_track[slot].birth = tick;
    sprite_track[slot].seen = tick;
    sprite_track[slot].life = s->life;

    ElRdDraw d;
    memset(&d, 0, sizeof d);
    d.id = make_id(EL_KIND_EFFECT, PASS_SPRITE, 0, slot, sprite_track[slot].birth);
    d.pos[0] = s->x; d.pos[1] = s->y; d.pos[2] = s->z;
    if (s->flags & 1) {
        /* Math_MtxFAlignYToVector(vec), pos, Math_MtxFScaleAxes(scaleX, scaleX, scaleX) */
        d.xform = EL_RDX_ALIGN_Y;
        d.rot[0] = s->vecX; d.rot[1] = s->vecY; d.rot[2] = s->vecZ;
        d.scale = s->scaleX;
    } else {
        /* Math_MtxFAimAt(pos, eye), the roll, Math_MtxFScaleAxes(scaleX, scaleY, 1) */
        d.xform = EL_RDX_AIM_EYE;
        d.rot[2] = s->roll;
        d.scale = s->scaleX;
        d.more[0] = s->scaleY;
    }
    d.inputs[0] = (uint32_t)(uintptr_t)s->tex; /* the flipbook frame: a snap */
    d.inputs[1] = s->life;
    draw(&d, (uint32_t)(uintptr_t)s->dl, intern(buf, (int)(g - buf)), mtx);
    counts[PASS_SPRITE]++;
}

/* ---- streaks, markers, rings ------------------------------------------------------------------------- */

static struct { uint32_t seen, birth; uint16_t left; } streak_track[32], marker_track[4];

static uint32_t birth_by_countdown(void *track_, int slot, uint16_t left) {
    struct { uint32_t seen, birth; uint16_t left; } *t = track_;
    if (t[slot].seen != tick - 1 || (uint16_t)(t[slot].left - 1) != left) t[slot].birth = tick;
    t[slot].seen = tick;
    t[slot].left = left;
    return t[slot].birth;
}

void nd_effect_streak(int slot, const EffectStreak *p, const void *mtx, float dist) {
    Gfx buf[ENTRY_CMDS], *g = base(buf);
    gSPDisplayList(g++, D_800EB880);
    ElRdDraw d;
    memset(&d, 0, sizeof d);
    d.id = make_id(EL_KIND_EFFECT, PASS_STREAK, 0, slot, birth_by_countdown(streak_track, slot, p->framesLeft));
    /* Math_MtxFAlignYToVector(velocity), Math_MtxFScaleAxes(0.35, dist * 0.1, 0.35), pos */
    d.xform = EL_RDX_ALIGN_Y_AXES;
    d.pos[0] = p->x; d.pos[1] = p->y; d.pos[2] = p->z;
    d.rot[0] = p->vx; d.rot[1] = p->vy; d.rot[2] = p->vz;
    d.scale = 0.35f;
    d.more[0] = (float)(dist * 0.1);
    d.more[1] = 0.35f;
    draw(&d, (uint32_t)(uintptr_t)D_800EB8E8, intern(buf, (int)(g - buf)), mtx);
    counts[PASS_STREAK]++;
}

void nd_effect_marker(int slot, int frames_left, const void *mtx, float x, float y, float z, float scale) {
    Gfx buf[ENTRY_CMDS], *g = base(buf);
    gSPDisplayList(g++, D_800EB990);
    ElRdDraw d;
    memset(&d, 0, sizeof d);
    d.id = make_id(EL_KIND_EFFECT, PASS_MARKER, 0, slot, birth_by_countdown(marker_track, slot, (uint16_t)frames_left));
    /* Math_MtxFAimAt(pos, eye), Math_MtxFScaleAxes(scale, scale, scale) */
    d.xform = EL_RDX_AIM_EYE;
    d.pos[0] = x; d.pos[1] = y; d.pos[2] = z;
    d.scale = scale;
    d.more[0] = scale;
    draw(&d, (uint32_t)(uintptr_t)D_800EB9C8, intern(buf, (int)(g - buf)), mtx);
    counts[PASS_MARKER]++;
}

static struct { uint32_t seen, birth; } ring_track[4];

void nd_effect_ring(int ring, int seg, const EffectRing *p, const void *mtx, float x0, float y0, float z0, float dx,
                    float dy, float dz, float len) {
    Gfx buf[ENTRY_CMDS], *g = base(buf);
    gSPDisplayList(g++, D_800C4780);
    gDPLoadTextureBlock(g++, D_800C47C0, G_IM_FMT_I, G_IM_SIZ_8b, 8, 8, 0, G_TX_WRAP, G_TX_WRAP, 3, 0, G_TX_NOLOD,
                        G_TX_NOLOD);
    /* a ring starts at its first growing frame (a fade restarts its frames: the same ring) */
    if (ring_track[ring].seen != tick) {
        if (ring_track[ring].seen != tick - 1 || (p->state == 1 && p->stateFrames == 1)) ring_track[ring].birth = tick;
        ring_track[ring].seen = tick;
    }
    ElRdDraw d;
    memset(&d, 0, sizeof d);
    d.id = make_id(EL_KIND_EFFECT, PASS_RING, seg, ring, ring_track[ring].birth);
    /* Math_MtxFAlignYToVector(the segment), Math_MtxFScaleAxes(2, len, 1.5), its start */
    d.xform = EL_RDX_ALIGN_Y_AXES;
    d.pos[0] = x0; d.pos[1] = y0; d.pos[2] = z0;
    d.rot[0] = dx; d.rot[1] = dy; d.rot[2] = dz;
    d.scale = 2.0f;
    d.more[0] = len;
    d.more[1] = 1.5f;
    draw(&d, (uint32_t)(uintptr_t)D_800C4840, intern(buf, (int)(g - buf)), mtx);
    counts[PASS_RING]++;
}

/* ---- model effects ------------------------------------------------------------------------------------ */

static const EffectModel *model;
static int model_slot;
static const void *model_entry;

/* Effect_ConfigureModelEffectTexture's commands as it queued them for `p` this tick (it has stepped the
 * scroll already), except the tile size: the scroll is the draw's tile offset (EL_RDM_TILE_SCROLL). The
 * TLUT is always loaded here (the game skips it when the last effect loaded the same one). */
static const void *model_entry_for(const EffectModel *p) {
    Gfx buf[ENTRY_CMDS], *g = base(buf);
    gSPDisplayList(g++, D_800EB6D0);
    const EffectTexAnim *t = &p->tex;
    s32 geom = 0;
    if (t->img != 0) {
        if (p->flags & 0x20) geom = G_LIGHTING;
        s32 img = t->img;
        u32 size = (t->flags & 1) ? 16 : 32;
        u32 mask = (t->flags & 1) ? 4 : 5;
        if (t->flags & 4) img += (p->age >> t->shift) * size * size;
        else if (t->flags & 8) img += ((p->age >> t->shift) & t->mask) * size * size;
        s32 clamp = (t->flags & 0x10) ? 1 : 0;
        if (t->flags & 2) {
            gDPLoadTextureBlock(g++, img, G_IM_FMT_I, G_IM_SIZ_8b, size, size, 0, clamp, clamp, mask, mask, G_TX_NOLOD,
                                G_TX_NOLOD);
            gDPSetTextureLUT(g++, G_TT_NONE);
            if (t->flags & 0x40) {
                if (p->flags & 0x20) {
                    gDPSetCombine(g++, 0x629A04, 0x1F10FFFF);
                } else {
                    gDPSetCombine(g++, 0x629BFF, 0x1F10FE3F);
                }
            } else if (p->flags & 0x20) {
                gDPSetCombine(g++, 0x62FE04, 0x1F10FBFF);
            } else {
                gDPSetCombine(g++, 0x62FFFF, 0x1F10FA3F);
            }
        } else {
            gDPLoadTextureBlock(g++, img, G_IM_FMT_CI, G_IM_SIZ_8b, size, size, 0, clamp, clamp, mask, mask, G_TX_NOLOD,
                                G_TX_NOLOD);
            gDPSetTextureLUT(g++, G_TT_RGBA16);
            if (p->flags & 0x20) {
                gDPSetCombine(g++, 0x129804, 0xFF17FFFF);
            } else {
                gDPSetCombine(g++, 0x1299FF, 0xFF17FE3F);
            }
            gDPLoadTLUT_pal256(g++, effect_model_tlut_ptrs[t->tlut & 3]);
        }
        gDPSetTileSize(g++, G_TX_RENDERTILE, 0, 0, size, size);
        if (t->flags & 0x20) {
            gSPTexture(g++, size << 6, size << 6, 0, G_TX_RENDERTILE, G_ON);
            geom |= G_LIGHTING | G_TEXTURE_GEN;
        } else {
            gSPTexture(g++, 0x8000, 0x8000, 0, G_TX_RENDERTILE, G_ON);
        }
    } else {
        gDPPipeSync(g++);
        gDPSetCombine(g++, 0x62CBFF, 0x4FFE7E38);
        gSPTexture(g++, 0x8000, 0x8000, 0, G_TX_RENDERTILE, G_OFF);
    }
    gSPClearGeometryMode(g++, G_LIGHTING | G_TEXTURE_GEN);
    if (geom) {
        gSPSetGeometryMode(g++, geom);
    }
    /* Effect_UpdateAndDrawModelEffects: culling */
    if (p->flags & 8) {
        gSPSetGeometryMode(g++, G_CULL_BACK);
    } else {
        gSPClearGeometryMode(g++, G_CULL_BACK);
    }
    return intern(buf, (int)(g - buf));
}

void nd_effect_model_begin(int slot, const EffectModel *p) {
    model = p;
    model_slot = slot;
    model_entry = model_entry_for(p);
}

void nd_effect_model_end(void) {
    model = NULL;
    tlut_rgba16 = 0; /* Effect_UpdateAndDrawModelEffects ends with no TLUT */
}

void nd_effect_node(int idx, int node_flags, const void *mtx, uint32_t dl, int frame) {
    if (!model || !dl) return;
    const EffectModel *p = model;
    ElRdDraw d;
    memset(&d, 0, sizeof d);
    d.id = make_id(EL_KIND_EFFECT, PASS_MODEL, idx, model_slot, tick - p->age);
    d.xform = EL_RDX_EFFECT_NODE;
    d.pos[0] = p->x; d.pos[1] = p->y; d.pos[2] = p->z;
    if (p->flags & 4) {
        d.rot[0] = p->upX; d.rot[1] = p->upY; d.rot[2] = p->upZ;
    } else {
        d.rot[1] = p->rotationY;
    }
    d.scale = 0.01f;
    d.more[0] = p->scaleX; d.more[1] = p->scaleY; d.more[2] = p->scaleZ;
    d.inputs[EL_RDI_ANIM_SET] = (uint32_t)(uintptr_t)&p->anim;
    d.inputs[EL_RDI_FRAME] = (uint32_t)frame;
    d.inputs[EL_RDI_NODE] = (uint32_t)(idx & 0xFF) | (uint32_t)(node_flags & 0xFF) << 8 | (uint32_t)(p->flags & 0xFF) << 16;
    d.inputs[EL_RDI_DL] = dl;
    /* the texture's scroll (the tile size the game queued), 12-bit as SETTILESIZE packs it */
    if (p->tex.img != 0) {
        d.tile_offset[0] = (float)(p->tex.s & 0xFFF);
        d.tile_offset[1] = (float)(p->tex.t & 0xFFF);
    }
    draw(&d, dl, model_entry, mtx);
    counts[PASS_MODEL]++;
}

/* ---- the screen: letterbox and fade ------------------------------------------------------------------ */

/* The letterbox: fade_letterbox_dl sets its state (1-cycle, ENV black, opaque) and fills 0..319 x 0..40
 * and 200..239. Here two quads in pixels with a matrix to clip space (EL_RDD_SCREEN); full width (the
 * fill rectangles stop a column short, under the N64's 8-pixel border). */
static Vtx letterbox_vtx[8];
static Gfx letterbox_model[6];
static const float letterbox_mtx[16] = {
    1.0f / 160.0f, 0, 0, 0,
    0, -1.0f / 120.0f, 0, 0,
    0, 0, 0, 0,
    -1.0f, 1.0f, 0, 1.0f,
};

static Gfx fade_entry[2][3], letterbox_entry[1];

void nd_effect_attach(void) {
    static const short xy[8][2] = { { 0, 0 }, { 320, 0 }, { 0, 40 }, { 320, 40 },
                                    { 0, 200 }, { 320, 200 }, { 0, 240 }, { 320, 240 } };
    memset(letterbox_vtx, 0, sizeof letterbox_vtx);
    for (int i = 0; i < 8; i++) {
        letterbox_vtx[i].v.ob[0] = xy[i][0];
        letterbox_vtx[i].v.ob[1] = xy[i][1];
        letterbox_vtx[i].v.cn[3] = 0xFF;
    }
    gSPEndDisplayList(letterbox_entry); /* nothing: fade_letterbox_dl sets all it uses */
    Gfx *g = letterbox_model;
    gSPDisplayList(g++, fade_letterbox_dl);
    gSPVertex(g++, letterbox_vtx, 8, 0);
    gSP2Triangles(g++, 0, 2, 1, 0, 1, 2, 3, 0);
    gSP2Triangles(g++, 4, 6, 5, 0, 5, 6, 7, 0);
    gSPEndDisplayList(g++);
    /* the fade quad: the render mode Fade_UpdateAndDraw sets (opaque at the ramp's end), then
     * fade_quad_dl (1-cycle, SHADE x ENV, no Z, no texture) */
    for (int k = 0; k < 2; k++) {
        g = fade_entry[k];
        gDPSetRenderMode(g++, k ? 0x0F0A4000 : 0x00504340, 0);
        gSPEndDisplayList(g++);
    }
}

void nd_screen_letterbox(void) {
    ElRdDraw d;
    memset(&d, 0, sizeof d);
    d.id = (uint64_t)EL_KIND_SCREEN << EL_RDD_KIND_SHIFT | (uint64_t)PASS_LETTERBOX << 48;
    d.flags = EL_RDD_SCREEN;
    d.xform = EL_RDX_MATRIX;
    memcpy(d.matrix, letterbox_mtx, sizeof d.matrix);
    draw(&d, (uint32_t)(uintptr_t)letterbox_model, letterbox_entry, NULL);
    screen_counts[PASS_LETTERBOX]++;
}

void nd_screen_fade(const void *ortho, const void *mv, int opaque) {
    float o[16], m[16];
    nd_decode_mtx(ortho, o);
    nd_decode_mtx(mv, m);
    ElRdDraw d;
    memset(&d, 0, sizeof d);
    d.id = (uint64_t)EL_KIND_SCREEN << EL_RDD_KIND_SHIFT | (uint64_t)PASS_FADE << 48;
    d.flags = EL_RDD_SCREEN;
    d.xform = EL_RDX_MATRIX;
    /* modelview x ortho (row vectors), as the RSP's stacks */
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += m[i * 4 + k] * o[k * 4 + j];
            d.matrix[i * 4 + j] = s;
        }
    draw(&d, (uint32_t)(uintptr_t)fade_quad_dl, fade_entry[opaque ? 1 : 0], NULL);
    screen_counts[PASS_FADE]++;
}
