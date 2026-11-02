/* A stand-in for libeltale_host.so in the memory-safety build (docs/MEMORY_SAFETY.md): no GPU, no window. With it
 * `--dump-frames DIR` turns the game's native draw side on (game/native_draw: model loader, recordings, 2D, the area
 * map's feed), so the sanitizers see that code run. Built with ASan by `make asan`: every buffer the game hands over
 * is read through in full (its reads are checked), and what the real renderer would trust is checked here: a mesh's
 * groups inside its vertices and materials, texel and palette ranges, ids handed out. A broken rule prints
 * "[asan] host stub: ..." (tools/asan_summary.py lists it). Buffer room as the real renderer's (port/render/
 * nd_renderer.cpp), ELTALE_ND_SMALL_BUFFERS=n KiB as there too, so resources_reset runs. The pictures it writes are
 * blank (the tools that read them go on); the picture's shape follows ELTALE_WIDESCREEN as the real headless one. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/render.h"
#include "port/automap.h"
#include "port/render/png.h"

#define API __attribute__((visibility("default")))

static size_t vertex_cap = (size_t)1 << 21, material_cap = (size_t)1 << 16, texel_cap = (size_t)1 << 24;
static size_t vertices_used, materials_used, texels_used;
static uint32_t mesh_count, frames, problems;
static uint32_t *mesh_vertices; /* each mesh's vertex count */
static size_t mesh_room;
static volatile uint32_t sink;

/* stderr, and the file ELTALE_ASAN_LOG names (tools/asan_run.sh) */
static void problem(const char *fmt, const char *what, unsigned long a, unsigned long b) {
    if (problems++ > 200) return;
    char text[200], line[260];
    snprintf(text, sizeof text, fmt, what, a, b);
    snprintf(line, sizeof line, "[asan] host stub: %s (frame %u)\n", text, frames);
    fputs(line, stderr);
    const char *path = getenv("ELTALE_ASAN_LOG");
    FILE *f = path ? fopen(path, "a") : NULL;
    if (f) {
        fputs(line, f);
        fclose(f);
    }
}

/* reads every byte (each read is checked by ASan) */
static void touch(const void *p, size_t bytes) {
    const unsigned char *c = p;
    uint32_t s = 0;
    for (size_t i = 0; i < bytes; i++) s += c[i];
    sink += s;
}

static void check_tile(const ElRdTile *t, const char *what) {
    if (t->format == EL_RDT_NONE) return;
    if (t->format > EL_RDT_CI) problem("%s: tile format %lu%.0lu", what, t->format, 0);
    size_t n = (size_t)t->width * t->height;
    if ((size_t)t->texels + n > texels_used) problem("%s: tile texels %lu.. past the %lu loaded", what, t->texels, texels_used);
}

static void check_material(const ElRdMaterial *m, const char *what) {
    if (m->flags & EL_RDM_TEXTURE) {
        check_tile(&m->tile[0], what);
        check_tile(&m->tile[1], what);
    }
}

static uint32_t texture_upload(const uint32_t *texels, uint32_t count) {
    touch(texels, (size_t)count * 4);
    if (texels_used + count > texel_cap) return ~0u;
    uint32_t at = (uint32_t)texels_used;
    texels_used += count;
    return at;
}

static uint32_t mesh_upload(const ElRdMeshDesc *d) {
    touch(d, sizeof *d);
    touch(d->vertices, (size_t)d->vertex_count * sizeof *d->vertices);
    touch(d->groups, (size_t)d->group_count * sizeof *d->groups);
    touch(d->materials, (size_t)d->material_count * sizeof *d->materials);
    for (uint32_t i = 0; i < d->group_count; i++) {
        const ElRdGroup *g = &d->groups[i];
        if ((uint64_t)g->first + g->count > d->vertex_count)
            problem("%s: group vertices %lu.. past the mesh's %lu", "mesh_upload", g->first + g->count, d->vertex_count);
        if (g->count % 3) problem("%s: group of %lu vertices%.0lu", "mesh_upload", g->count, 0);
        if (g->material >= d->material_count)
            problem("%s: group material %lu of %lu", "mesh_upload", g->material, d->material_count);
    }
    for (uint32_t i = 0; i < d->material_count; i++) check_material(&d->materials[i], "mesh_upload");
    if ((vertices_used + d->vertex_count) * sizeof(ElRdVertex) > vertex_cap * sizeof(ElRdVertex) ||
        (materials_used + d->material_count) > material_cap)
        return ~0u;
    vertices_used += d->vertex_count;
    materials_used += d->material_count;
    if (mesh_count == mesh_room) {
        mesh_room = mesh_room ? mesh_room * 2 : 1024;
        mesh_vertices = realloc(mesh_vertices, mesh_room * sizeof *mesh_vertices);
    }
    mesh_vertices[mesh_count] = d->vertex_count;
    return mesh_count++;
}

static void palette(uint32_t id, const uint16_t *entries, uint32_t count) {
    touch(entries, (size_t)count * 2);
    if (count > 256) problem("%s: %lu entries for palette %lu", "palette", count, id);
}

static void resources_reset(void) { vertices_used = materials_used = texels_used = 0, mesh_count = 0; }
static void frame_begin(const ElRdFrameInfo *info) { touch(info, sizeof *info); }
static void camera(const ElRdCamera *c) { touch(c, sizeof *c); }
static void lights(const ElRdLights *l) {
    touch(l, sizeof *l);
    if (l->count > 7) problem("%s: %lu directional lights%.0lu", "lights", l->count, 0);
}
static void colour(const uint8_t rgba[4]) { touch(rgba, 4); }
static void fog_position(int16_t m, int16_t o) { (void)m, (void)o; }
static void draw(const ElRdDraw *d) {
    touch(d, sizeof *d);
    if (d->mesh >= mesh_count) problem("%s: mesh %lu of %lu", "draw", d->mesh, mesh_count);
    if ((d->id >> EL_RDD_KIND_SHIFT) >= EL_KIND_COUNT) problem("%s: kind %lu%.0lu", "draw", (unsigned long)(d->id >> 56), 0);
}
/* --dump-frames: a blank picture of the right size for each frame the spec names (as port/render/nd_renderer.cpp's
 * want_dump), so the test tools that read them go on */
static char dump_dir[1024], dump_spec[1024];
static float aspect(void);
static int want_dump(uint32_t frame) {
    if (!dump_dir[0]) return 0;
    if (!dump_spec[0]) return 1;
    for (const char *p = dump_spec; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL) {
        char *e = NULL, *e2 = NULL;
        unsigned long first = strtoul(p, &e, 10);
        if (*e == '-') {
            if (frame >= first && frame <= strtoul(e + 1, NULL, 10)) return 1;
        } else if (*e == '+') {
            unsigned long step = strtoul(e + 1, &e2, 10), run = *e2 == '/' ? strtoul(e2 + 1, NULL, 10) : 1;
            if (frame >= first && step && (frame - first) % step < run) return 1;
        } else if (frame == first) {
            return 1;
        }
    }
    return 0;
}
static void frame_end(uint32_t frame) {
    frames++;
    if (!want_dump(frame)) return;
    uint32_t w = (uint32_t)(aspect() * 240.0f + 0.5f), h = 240;
    uint32_t *px = calloc((size_t)w * h, 4);
    char path[1100];
    snprintf(path, sizeof path, "%s/frame_%u.png", dump_dir, frame);
    if (px) render_png_write(path, px, w, h);
    free(px);
}
static uint32_t material_2d(const ElRdMaterial *m) {
    touch(m, sizeof *m);
    check_material(m, "material_2d");
    if (materials_used + 1 > material_cap) return ~0u;
    return (uint32_t)materials_used++;
}
static void draw_2d(const ElRd2D *item) {
    touch(item, sizeof *item);
    if (item->mesh != EL_RD2D_RECT && item->mesh >= mesh_count) problem("%s: mesh %lu of %lu", "draw_2d", item->mesh, mesh_count);
    if (item->mesh == EL_RD2D_RECT && !(item->flags & EL_RD2D_FILL) && item->material >= materials_used)
        problem("%s: material %lu of %lu", "draw_2d", item->material, materials_used);
}
static void frame_phase(float phase) { (void)phase; }
static float draw_distance(void) { return 1.0f; }
/* the picture's shape as the real renderer's headless dumps take it (render_native.cpp wanted_aspect, the 1x size) */
static float aspect(void) {
    const char *e = getenv("ELTALE_WIDESCREEN");
    float a = 4.0f / 3.0f;
    if (e && (!strcmp(e, "16:9") || !strcmp(e, "1") || !strcmp(e, "window") || !strcmp(e, "2"))) a = 16.0f / 9.0f;
    else if (e && strcmp(e, "off") && strcmp(e, "4:3") && strcmp(e, "0") && atof(e) > 0) a = (float)atof(e);
    a = a < 4.0f / 3.0f ? 4.0f / 3.0f : a > 32.0f / 9.0f ? 32.0f / 9.0f : a;
    uint32_t w = 2 * (uint32_t)lroundf(a * 240.0f * 0.5f);
    return w <= 320 ? 4.0f / 3.0f : (float)w / 240.0f;
}
static void reset(void) { resources_reset(); }

static const ElRenderApi api = {
    EL_RENDER_API_VERSION, texture_upload, mesh_upload, palette, resources_reset, frame_begin, camera, lights,
    colour, colour, colour, fog_position, draw, frame_end, material_2d, draw_2d, frame_phase, draw_distance, aspect, reset,
};

API int ElHost_InitHeadless(void) {
    const char *e = getenv("ELTALE_ND_SMALL_BUFFERS");
    if (e && atol(e) > 0) {
        size_t bytes = (size_t)atol(e) * 1024;
        if (bytes / sizeof(ElRdVertex) < vertex_cap) vertex_cap = bytes / sizeof(ElRdVertex);
        if (bytes / 128 < material_cap) material_cap = bytes / 128; /* (the real GpuMaterial's size, about) */
        if (bytes / 4 < texel_cap) texel_cap = bytes / 4;
    }
    fprintf(stderr, "[asan] host stub up (no pictures)\n");
    return 0;
}
API const void *ElHost_RenderApi(void) { return &api; }
API void ElHost_SetDump(const char *dir, const char *spec) {
    snprintf(dump_dir, sizeof dump_dir, "%s", dir ? dir : "");
    snprintf(dump_spec, sizeof dump_spec, "%s", spec ? spec : "");
}
API void ElHost_Shutdown(void) {
    fprintf(stderr, "[asan] host stub: %u frames, %u problems\n", frames, problems);
}

API void ElHost_AutomapArea(const ElAmArea *a) {
    touch(a, sizeof *a);
    if (a->version != EL_AM_VERSION) problem("%s: version %lu%.0lu", "automap", a->version, 0);
    touch(a->tris, (size_t)a->tri_count * sizeof *a->tris);
    touch(a->walls, (size_t)a->wall_count * sizeof *a->walls);
    touch(a->exits, (size_t)a->exit_count * sizeof *a->exits);
    touch(a->water, (size_t)a->water_count * sizeof *a->water);
    touch(a->zones, (size_t)a->zone_count * sizeof *a->zones);
    touch(a->points, (size_t)a->point_count * 2 * sizeof *a->points);
    for (uint32_t i = 0; i < a->zone_count; i++) {
        if (a->zones[i].group_count > 7) problem("%s: zone with %lu groups%.0lu", "automap", a->zones[i].group_count, 0);
        if (!memchr(a->zones[i].label, 0, sizeof a->zones[i].label)) problem("%s: zone label unterminated%.0lu%.0lu", "automap", 0, 0);
    }
}
API void ElHost_AutomapPlayer(const ElAmPlayer *p) {
    touch(p, sizeof *p);
    touch(p->markers, (size_t)p->marker_count * sizeof *p->markers);
    for (uint32_t i = 0; i < p->marker_count; i++) {
        const ElAmMarker *m = &p->markers[i];
        if (!memchr(m->label, 0, sizeof m->label) || !memchr(m->detail, 0, sizeof m->detail))
            problem("%s: marker %lu text unterminated%.0lu", "automap", i, 0);
    }
}
API void ElHost_AutomapSave(int kind, uint64_t key) { (void)kind, (void)key; }
