/* native_draw.c: the adapter between the game's draw code and the native renderer (native_draw.h,
 * port/render.h, docs/renderer/NATIVE_DRAW.md). PC-only.
 *
 * It knows the game's structs (GfxFrameData, MapObj, Lights3, the camera's globals) and turns what a
 * draw site draws into the renderer's calls: a mesh (nd_model.c makes it from the static display list
 * once), a matrix (the very Mtx the RSP would load, decoded), and the state of the moment.
 *
 * Switches (nd_config):
 *   --native-kinds LIST / ELTALE_NATIVE_KINDS=LIST   which kinds the native renderer draws: "all"
 *       (the default), "none", or names (map,billboard,backdrop,actor,shadow,effect,screen,hud,text,
 *       menu,picture). Only kinds that are done draw natively (R2: map).
 *   ELTALE_ND_COMPARE=1    native kinds keep their display-list commands too (one run gives both the
 *       frame image for the reference and the native picture)
 *   ELTALE_DL_KINDS=LIST   the frame list keeps only these kinds' commands (the rest are dropped after
 *       they are queued: the game runs the same). For the reference of one kind alone. */
#include "game/native_draw/native_draw.h"
#include "game/native_draw/nd_model.h"

#include "game/core/main.h"
#include "game/camera/camera.h"
#include "game/map/map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned nd_kinds;
unsigned nd_dl_kinds = ~0u;

/* the kinds the native renderer can draw so far */
#define ND_DONE (1u << EL_KIND_MAP)

static const ElRenderApi *api;
static unsigned requested = ~0u;
static int compare;
static int in_frame;

static const char *const kind_names[EL_KIND_COUNT] = {
    "map", "billboard", "backdrop", "actor", "shadow", "effect", "screen", "hud", "text", "menu", "picture",
};

static unsigned parse_kinds(const char *s) {
    if (!s || !*s || !strcmp(s, "all")) return ~0u;
    if (!strcmp(s, "none")) return 0;
    unsigned m = 0;
    while (*s) {
        const char *e = strchr(s, ',');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        int found = 0;
        for (int k = 0; k < EL_KIND_COUNT; k++)
            if (strlen(kind_names[k]) == n && !strncmp(s, kind_names[k], n)) m |= 1u << k, found = 1;
        if (!found) fprintf(stderr, "native draw: unknown kind '%.*s'\n", (int)n, s);
        s += n + (e ? 1 : 0);
    }
    return m;
}

static void update_switches(void) {
    nd_kinds = api ? requested & ND_DONE : 0;
    nd_dl_kinds = parse_kinds(getenv("ELTALE_DL_KINDS"));
    if (!compare) nd_dl_kinds &= ~nd_kinds;
}

void nd_config(int argc, char **argv) {
    const char *kinds = getenv("ELTALE_NATIVE_KINDS");
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--native-kinds")) kinds = argv[i + 1];
    requested = parse_kinds(kinds);
    compare = getenv("ELTALE_ND_COMPARE") != NULL;
    update_switches();
}

/* Map object entry states (built in nd_attach): what the frame list has set when it calls a model */
static Gfx entry_map[8], entry_xlu[8], entry_scroll[5][24];

extern Gfx D_10000E8[];
extern u8 *mapobj_scrolltex_tex[5];
extern u16 mapobj_scrolltex_height[6];

static void build_entries(void) {
    Gfx *g = entry_map;
    /* Gfx_BuildFrameDisplayList: 2-cycle; MapObj_UpdateAndDrawNearby: the map's setup list */
    gDPSetCycleType(g++, G_CYC_2CYCLE);
    gSPDisplayList(g++, D_10000E8);
    gSPEndDisplayList(g++);
    /* MapObj_DrawTranslucent (after MapObj_UpdateAndDrawNearby set bilinear filtering and no TLUT) */
    g = entry_xlu;
    gSPDisplayList(g++, entry_map);
    gDPSetTextureLUT(g++, G_TT_NONE);
    gDPSetRenderMode(g++, 0x0C1849D8, 0);
    gDPSetTextureFilter(g++, G_TF_BILERP);
    gSPEndDisplayList(g++);
    for (int k = 0; k < 5; k++) {
        u32 h = mapobj_scrolltex_height[k];
        g = entry_scroll[k];
        gSPDisplayList(g++, entry_xlu);
        gDPSetRenderMode(g++, 0xC81049D8, 0);
        gDPSetCombine(g++, 0x121A04, 0xFFFFFFF8);
        gDPLoadTextureTile(g++, mapobj_scrolltex_tex[k], G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, h, 0, 0, 31, h - 1, 0, G_TX_WRAP,
                           G_TX_WRAP, 5, (h == 32) ? 5 : 6, G_TX_NOLOD, G_TX_NOLOD);
        /* the scroll offset is the draw's (ElRdDraw.tile_offset) */
        gDPSetTileSize(g++, G_TX_RENDERTILE, 0, 0, 32 << 2, h << 2);
        gSPEndDisplayList(g++);
    }
}

void nd_attach(const ElRenderApi *a) {
    if (!a || a->version != EL_RENDER_API_VERSION) {
        fprintf(stderr, "native draw: the renderer's interface is not this build's\n");
        return;
    }
    api = a;
    nd_model_init(api);
    build_entries();
    update_switches();
    fprintf(stderr, "native draw: kinds %x drawn natively, display lists kept for %x%s\n", nd_kinds, nd_dl_kinds,
            compare ? " (compare)" : "");
}

void nd_after_reset(void) {
    if (api) nd_model_reset();
}

void nd_forget(uintptr_t host, uint32_t bytes) {
    if (api) nd_model_forget(host, bytes);
}

/* ---- the tick ---------------------------------------------------------------------------------- */

void nd_frame_begin(void) {
    if (!nd_kinds) return;
    ElRdFrameInfo info;
    memset(&info, 0, sizeof info);
    /* the clear rectangle's colour (gameplay modes; the rest clear to black): RGBA5551 as the RDP fills it */
    if (core_game_mode == 1 || core_game_mode == 6 || core_game_mode == 7) {
        u16 c = (u16)GPACK_RGBA5551(gfx_clear_r, gfx_clear_g, gfx_clear_b, 1);
        info.clear[0] = (u8)((c >> 11 & 31) << 3 | (c >> 11 & 31) >> 2);
        info.clear[1] = (u8)((c >> 6 & 31) << 3 | (c >> 6 & 31) >> 2);
        info.clear[2] = (u8)((c >> 1 & 31) << 3 | (c >> 1 & 31) >> 2);
    }
    info.mode = (u8)core_game_mode;
    api->frame_begin(&info);
    in_frame = 1;
}

void nd_frame_end(unsigned frame) {
    if (!nd_kinds || !in_frame) return;
    in_frame = 0;
    nd_model_frame_end();
    api->frame_end(frame);
}

/* ---- state -------------------------------------------------------------------------------------- */

/* A libultra Mtx (s15.16: the integer halves then the fractions) as floats, m[row][col] */
static void decode_mtx(const void *mtx, float *m) {
    const u32 *w = (const u32 *)mtx;
    for (int i = 0; i < 16; i++) {
        u32 hi = (w[i / 2] >> (i % 2 ? 0 : 16)) & 0xFFFF, lo = (w[8 + i / 2] >> (i % 2 ? 0 : 16)) & 0xFFFF;
        m[i] = (float)(s32)(hi << 16 | lo) / 65536.0f;
    }
}

/* c = a * b (row vectors: v * a * b) */
static void mul(const float *a, const float *b, float *c) {
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = s;
        }
    memcpy(c, r, sizeof r);
}

void nd_camera(struct GfxFrameData *f, const void *shake_mtx) {
    if (!nd_kinds) return;
    ElRdCamera c;
    memset(&c, 0, sizeof c);
    float view[16], persp[16], shake[16];
    decode_mtx(&f->proj, persp);
    decode_mtx(&f->view, view);
    mul(view, persp, c.projection);
    if (shake_mtx) {
        decode_mtx(shake_mtx, shake);
        mul(c.projection, shake, c.projection);
        c.shake[0] = shake[12];
        c.shake[1] = shake[13];
    }
    memcpy(c.view, view, sizeof view);
    memcpy(c.perspective, persp, sizeof persp);
    for (int i = 0; i < 3; i++) {
        c.lookat_x[i] = f->lookAt.l[0].l.dir[i];
        c.lookat_y[i] = f->lookAt.l[1].l.dir[i];
    }
    c.persp_norm = camera_persp_norm;
    c.near_plane = camera_near;
    c.far_plane = camera_far;
    c.fovy = camera_fovy;
    c.aspect = 4.0f / 3.0f;
    c.eye[0] = camera_state.eyeX; c.eye[1] = camera_state.eyeY; c.eye[2] = camera_state.eyeZ;
    c.at[0] = camera_state.atX; c.at[1] = camera_state.atY; c.at[2] = camera_state.atZ;
    api->camera(&c);
}

void nd_lights(const void *lights3) {
    if (!nd_kinds) return;
    const Lights3 *l = lights3;
    ElRdLights o;
    memset(&o, 0, sizeof o);
    memcpy(o.ambient, l->a.l.col, 3);
    o.count = 3;
    for (int i = 0; i < 3; i++) {
        memcpy(o.color[i], l->l[i].l.col, 3);
        for (int k = 0; k < 3; k++) o.dir[i][k] = l->l[i].l.dir[k];
    }
    api->lights(&o);
}

static void color(void (*fn)(const uint8_t *), int r, int g, int b, int a) {
    if (!nd_kinds) return;
    const uint8_t c[4] = { (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a };
    fn(c);
}

void nd_env_color(int r, int g, int b, int a) { if (nd_kinds) color(api->env_color, r, g, b, a); }
void nd_prim_color(int r, int g, int b, int a) { if (nd_kinds) color(api->prim_color, r, g, b, a); }
void nd_fog_color(int r, int g, int b, int a) { if (nd_kinds) color(api->fog_color, r, g, b, a); }

void nd_fog_position(int near_, int far_) {
    if (!nd_kinds) return;
    /* gSPFogPosition's arithmetic */
    api->fog_position((int16_t)(128000 / (far_ - near_)), (int16_t)((500 - near_) * 256 / (far_ - near_)));
}

/* ---- map objects ------------------------------------------------------------------------------ */

void nd_map_draw(const struct MapObj *obj, int pass, const void *mtx, uint32_t dl, int xform, float x, float y,
                 float z, float rx, float ry, float rz, float tile_t) {
    const void *entry = pass == ND_MAP_OPAQUE ? (const void *)entry_map
                      : pass == ND_MAP_XLU ? (const void *)entry_xlu
                      : (const void *)entry_scroll[pass - ND_MAP_SCROLL];
    const NdMesh *mesh = nd_model_get(dl, entry);
    if (!mesh) return;
    nd_model_use(mesh);
    ElRdDraw d;
    memset(&d, 0, sizeof d);
    d.id = (uint64_t)EL_KIND_MAP << EL_RDD_KIND_SHIFT | (uint64_t)pass << 48 | ((uint64_t)(uintptr_t)obj & 0xFFFFFFFFFFFFull);
    d.mesh = nd_model_id(mesh);
    decode_mtx(mtx, d.matrix);
    d.xform = (uint32_t)xform;
    d.pos[0] = x; d.pos[1] = y; d.pos[2] = z;
    d.rot[0] = rx; d.rot[1] = ry; d.rot[2] = rz;
    d.scale = obj->scale;
    d.tile_offset[1] = tile_t;
    api->draw(&d);
}
