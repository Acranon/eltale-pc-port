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
#include "game/native_draw/nd_2d.h"

#include "game/core/main.h"
#include "game/camera/camera.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned nd_kinds;
unsigned nd_dl_kinds = ~0u;
float nd_aspect = 1.3333334f;

static const ElRenderApi *api;

const ElRenderApi *nd_api(void) { return api; }

/* The kinds done so far (nd_internal.h: each kind's file adds one row) */
const NdKind nd_kinds_table[] = {
    { EL_KIND_MAP, nd_map_attach },
    { EL_KIND_BILLBOARD, nd_billboard_attach },
    { EL_KIND_BACKDROP, nd_backdrop_attach },
    { EL_KIND_SHADOW, nd_shadow_attach },
    { EL_KIND_ACTOR, nd_actor_attach },
    { EL_KIND_EFFECT, nd_effect_attach },
    { EL_KIND_SCREEN, NULL },
    { EL_KIND_HUD, NULL },     /* nd_2d.c */
    { EL_KIND_TEXT, NULL },    /* nd_2d.c */
    { EL_KIND_MENU, NULL },    /* nd_2d.c */
    { EL_KIND_PICTURE, NULL }, /* nd_2d.c */
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
    if (api) nd_model_reset_all();
}

void nd_forget(uintptr_t host, uint32_t bytes) {
    if (api) nd_model_forget(host, bytes), nd_2d_forget(host, bytes);
}

/* ---- the frame list: what is left of it (ELTALE_DL_CENSUS) ---------------------------------------- */

static long kept_frame[EL_KIND_COUNT];
static int census = -1;
static unsigned long long census_frames, census_frames_left, census_kind[EL_KIND_COUNT], census_state;

static void census_print(void) {
    printf("P1_DL_CENSUS frames=%llu frames_with_commands=%llu", census_frames, census_frames_left);
    unsigned long long total = census_state;
    for (int k = 0; k < EL_KIND_COUNT; k++) total += census_kind[k];
    printf(" commands=%llu", total);
    for (int k = 0; k < EL_KIND_COUNT; k++)
        if (census_kind[k]) printf(" %s=%llu", kind_names[k], census_kind[k]);
    printf(" state=%llu\n", census_state);
    fflush(stdout);
}

void nd_dl_kept(int kind, long count) {
    if (kind >= 0 && kind < EL_KIND_COUNT && count > 0) kept_frame[kind] += count;
}

void nd_frame_list_end(void *list_start, void **head) {
    if (!nd_kinds) return;
    long kept = 0;
    for (int k = 0; k < EL_KIND_COUNT; k++) kept += kept_frame[k];
    /* every command the kinds queued went to the native renderer: the frame's set-up (segments, the
     * viewport, the clears: the native renderer clears by itself) and the state the kinds sent from where
     * it was queued (camera, lights, fog) are not needed either */
    if (!kept && !compare) *head = list_start;
    if (census < 0) {
        census = getenv("ELTALE_DL_CENSUS") != NULL;
        if (census) atexit(census_print);
    }
    if (census) {
        long left = (long)(((uint8_t *)*head - (uint8_t *)list_start) / 8);
        census_frames++;
        if (left) census_frames_left++;
        long state = left;
        for (int k = 0; k < EL_KIND_COUNT; k++) census_kind[k] += (unsigned long long)kept_frame[k], state -= kept_frame[k];
        census_state += (unsigned long long)(state > 0 ? state : 0);
    }
    memset(kept_frame, 0, sizeof kept_frame);
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
    /* R7: the picture's shape (4:3 unless widescreen); kept for the whole tick */
    nd_aspect = api->aspect ? api->aspect() : 1.3333334f;
    if (!(nd_aspect >= 1.3333334f && nd_aspect < 8.0f)) nd_aspect = 1.3333334f;
    api->frame_begin(&info);
    in_frame++; /* the next frame can be built before the last one's task starts (menus, the title) */
    nd_2d_frame_begin();
}

void nd_frame_end(unsigned frame) {
    if (!nd_kinds || !in_frame) return;
    in_frame--;
    nd_model_frame_end();
    /* the virtual clock's place in its retrace (port/os/kernel.c: retraces every 781,250 counts from 0) */
    api->frame_phase((float)(osGetTime() % 781250u) / 781250.0f);
    api->frame_end(frame);
}

/* R7: MapObj_DrawBillboards draws a billboard when the angle between the camera's yaw and the way to it is
 * under fovy degrees: the half-width of a 4:3 picture plus a margin. A wider picture shows more at each
 * side: the cone grows by that much (the half-width at this aspect minus the half-width at 4:3). */
float nd_billboard_cone(float fovy) {
    if (!ND_WIDE()) return fovy;
    const double t = tan(fovy * 0.008726646259971648); /* tan(fovy / 2) */
    const double extra = atan(t * nd_aspect) - atan(t * (4.0 / 3.0));
    return fovy + (float)(extra * 57.29577951308232);
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
    c.aspect = nd_aspect;
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

/* (the last env colour sent: R7b's far translucent objects put it back after theirs) */
static uint8_t env_last[4];

void nd_env_color(int r, int g, int b, int a) {
    if (!nd_kinds) return;
    env_last[0] = (uint8_t)r, env_last[1] = (uint8_t)g, env_last[2] = (uint8_t)b, env_last[3] = (uint8_t)a;
    color(api->env_color, r, g, b, a);
}

void nd_env_last(uint8_t rgba[4]) { memcpy(rgba, env_last, 4); }
void nd_prim_color(int r, int g, int b, int a) { if (nd_kinds) color(api->prim_color, r, g, b, a); }
void nd_fog_color(int r, int g, int b, int a) { if (nd_kinds) color(api->fog_color, r, g, b, a); }

void nd_fog_position(int near_, int far_) {
    if (!nd_kinds) return;
    /* gSPFogPosition's arithmetic */
    api->fog_position((int16_t)(128000 / (far_ - near_)), (int16_t)((500 - near_) * 256 / (far_ - near_)));
}
