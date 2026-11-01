/* native_draw.c: the adapter between the game's draw code and the native renderer (native_draw.h,
 * port/render.h, docs/renderer/NATIVE_DRAW.md). PC-only. This is the core: the switches, the tick and
 * the state the game queues (camera, lights, colours, fog). Each kind's draws are in a file of its own
 * (nd_map.c, ...), listed in nd_kinds_table.
 *
 * It knows the game's structs (GfxFrameData, Lights3, the camera's globals) and turns what the game
 * queues into the renderer's calls; a kind's file turns a draw site's draw into a mesh (nd_model.c
 * makes it from the static display list once), a matrix (the very Mtx the RSP would load, decoded) and
 * the state of the moment.
 *
 * Switches (nd_config):
 *   --native-kinds LIST / ELTALE_NATIVE_KINDS=LIST   which kinds the native renderer draws: "all"
 *       (the default), "none", or names (map,billboard,backdrop,actor,shadow,effect,screen,hud,text,
 *       menu,picture). Only kinds that are done draw natively (nd_kinds_table; R2: map).
 *   ELTALE_ND_COMPARE=1    native kinds keep their display-list commands too (one run gives both the
 *       frame image for the reference and the native picture)
 *   ELTALE_DL_KINDS=LIST   the frame list keeps only these kinds' commands (the rest are dropped after
 *       they are queued: the game runs the same). For the reference of one kind alone. */
#include "game/native_draw/native_draw.h"
#include "game/native_draw/nd_internal.h"

#include "game/core/main.h"
#include "game/camera/camera.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned nd_kinds;
unsigned nd_dl_kinds = ~0u;

static const ElRenderApi *api;

const ElRenderApi *nd_api(void) { return api; }

/* The kinds done so far (nd_internal.h: each kind's file adds one row) */
const NdKind nd_kinds_table[] = {
    { EL_KIND_MAP, nd_map_attach },
    { EL_KIND_BACKDROP, nd_backdrop_attach },
    { -1, NULL },
};
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
    unsigned done = 0;
    for (const NdKind *k = nd_kinds_table; k->kind >= 0; k++) done |= 1u << k->kind;
    nd_kinds = api ? requested & done : 0;
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

void nd_attach(const ElRenderApi *a) {
    if (!a || a->version != EL_RENDER_API_VERSION) {
        fprintf(stderr, "native draw: the renderer's interface is not this build's\n");
        return;
    }
    api = a;
    nd_model_init(api);
    for (const NdKind *k = nd_kinds_table; k->kind >= 0; k++)
        if (k->attach) k->attach();
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
void nd_decode_mtx(const void *mtx, float *m) {
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
    nd_decode_mtx(&f->proj, persp);
    nd_decode_mtx(&f->view, view);
    mul(view, persp, c.projection);
    if (shake_mtx) {
        nd_decode_mtx(shake_mtx, shake);
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
