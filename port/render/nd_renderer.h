// The native renderer's core (docs/renderer/NATIVE_DRAW.md): the resources the model loader uploads
// (port/include/port/render.h), the recordings the game makes each tick, and drawing a recording into
// a picture with the shaders in port/render/shaders/.
#pragma once

#include "device.hpp"

#include "port/render.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace render {

struct DrawOptions {
    bool border = false;      // the N64's 8-pixel border: scissor 304x224 inside 320x240
    bool three_point = true;  // N64 three-point filtering (else four-point bilinear)
    bool n64_fog = false;     // the N64's fog factor, per vertex (else per pixel)
    // R7 (modern options): the picture's size is the caller's (320x240 times the resolution scale);
    uint32_t samples = 1;        // MSAA samples (1, 2, 4, 8): drawn multisampled, resolved into the picture
    float draw_distance = 1.0f;  // the fog and the far plane this many times as far (1: the game's)
    bool n64_dither = false;     // the RDP's dither: the picture in 5 bits a channel with its magic square
};

// One tick: what the game drew, in order, and the state each draw was made with
struct Recording {
    struct State {
        ElRdLights lights;
        uint8_t env[4], prim[4], fog[4], blend[4];
        int16_t fog_mul, fog_off;
    };
    // a run-time palette the tick changed (R6: a picture of an older recording puts its palettes back)
    struct PaletteChange {
        uint32_t id, count;
        uint32_t before[256], after[256];
    };
    struct Item {
        ElRdDraw draw;
        uint32_t state;
        int32_t item2d = -1; // a 2D item (items2d): the draw is unused
    };
    ElRdFrameInfo info = {};
    uint64_t vi = 0;     // the retraces presented when it was finished (its time: R6's pictures between)
    uint64_t serial = 0; // counts recordings
    double time = 0;     // vi plus the game's virtual clock's phase in that retrace at frame_end
    ElRdCamera camera = {};
    bool has_camera = false;
    std::vector<State> states;
    std::vector<Item> items;
    std::vector<ElRd2D> items2d;
    std::vector<PaletteChange> palettes; // in the order they were made
    uint32_t frame = 0;
};

class Interp;
struct InterpStats;

// Pictures between ticks (R6): what a picture at a time showed (NativeDraw::picture)
struct PictureStats {
    uint64_t newest = 0;  // the newest recording as it is (its own time, or no newer one yet)
    uint64_t between = 0; // a picture between two recordings
    uint64_t fallback = 0, early = 0, gap = 0, mode = 0, camera = 0, cut = 0; // shown as is: why
};

// The resources' lifetime (docs/renderer/NATIVE_DRAW.md "Resets and full buffers")
struct LifetimeStats {
    uint64_t resets = 0;  // the game's Reset (reset): resources and recordings dropped
    uint64_t flushes = 0; // full buffers (resources_reset): the recordings' resources kept, the rest dropped
    uint64_t kept_meshes = 0, kept_materials = 0, kept_texels = 0; // what the flushes kept, summed
    uint64_t stale = 0;   // items that named a mesh, group or material that is not there (skipped: 0)
};

class NativeDraw {
public:
    bool init(Vulkan::Device &device);
    void shutdown();
    // the function table the game calls (one renderer per process)
    const ElRenderApi *api();

    // the newest finished recording (null before the first)
    const Recording *latest() const { return hist.empty() ? nullptr : &hist.back(); }
    // The picture for the time v, in retraces presented (set_vi's count, with the fraction since the
    // last): a recording is due BETWEEN_DELAY retraces after it was finished (its time: the retrace and the
    // game's virtual clock's phase in it), and between two due
    // times the picture is the pair's picture between (nd_interp.h). `between` false: the newest as is.
    const Recording *picture(double v, bool between);
    void set_vi(uint64_t vi) { vi_now = vi; }
    const PictureStats &picture_stats() const { return pstats; }
    const InterpStats &interp_stats() const;
    const LifetimeStats &lifetime_stats() const { return life; }
    void print_stats() const;
    // two retraces (a gameplay tick) and a quarter for the time the game's thread takes to build one
    static constexpr double BETWEEN_DELAY = 2.25;
    // Draw `rec` into `picture` (RGBA8, GENERAL layout: picture_size_1x for the aspect, 320x240 at 4:3, times
    // the resolution scale; its shape is the picture's), with its own depth buffer (and with opt.samples > 1
    // its own multisampled colour, resolved into the picture)
    void render(Vulkan::CommandBuffer &cmd, Vulkan::Image &picture, const Recording *rec, const DrawOptions &opt);

    // Headless pictures for the compare tools: frames whose number is in `spec` (as ELTALE_DUMP_IMAGE:
    // "a,b,c" or "first+step"; empty = every frame) are drawn at 1x with the N64 look and written as
    // dir/frame_N.png when they end.
    void set_dump(const std::string &dir, const std::string &spec);

    // ElRenderApi
    uint32_t texture_upload(const uint32_t *texels, uint32_t count);
    uint32_t mesh_upload(const ElRdMeshDesc *mesh);
    void palette(uint32_t id, const uint16_t *entries, uint32_t count);
    void resources_reset();
    void reset();
    void frame_begin(const ElRdFrameInfo *info);
    void camera(const ElRdCamera *camera);
    void lights(const ElRdLights *lights);
    void env_color(const uint8_t *c);
    void prim_color(const uint8_t *c);
    void fog_color(const uint8_t *c);
    void fog_position(int16_t mul, int16_t off);
    void draw(const ElRdDraw *draw);
    void frame_end(uint32_t frame);
    uint32_t material_2d(const ElRdMaterial *material);
    void draw_2d(const ElRd2D *item);
    void frame_phase(float p) { phase = p; }
    // R7: the draw distance (1 the game's): the game's map pass reads it each tick (render.h draw_distance)
    // and the pictures push the fog and far plane by it. ELTALE_DRAW_DISTANCE=k fixes it (tests, the dumps).
    float draw_distance() const { return distance; }
    void set_draw_distance(float k) {
        if (!distance_forced) distance = k < 1.0f ? 1.0f : k;
    }
    // R7 (widescreen): the picture's shape the game builds its next ticks for (width / height; 4/3 exactly
    // is the N64's). The host sets it from the Graphics page (render_native.cpp), headless from
    // ELTALE_WIDESCREEN.
    float aspect() const { return aspect_value; }
    void set_aspect(float a) { aspect_value = a; }
    // The picture's size in pixels at 1x for an aspect: 240 high, the width an even number of pixels
    // (so the 4:3 middle starts on a whole pixel: 2D stays pixel-exact), at least 320
    static void picture_size_1x(float aspect, uint32_t *w, uint32_t *h);
    // The aspect a 1x picture of picture_size_1x has (what the game is told: its perspective matches the
    // picture exactly)
    static float aspect_of_1x(float aspect);

private:
    struct Mesh {
        uint32_t first, vertex_count;     // vertices
        uint32_t group_first, group_count;
        uint32_t material_first, material_count; // its own materials (groups name them)
        float bounds[6];
    };
    struct Texture {
        uint32_t first, count; // an upload's texels (the texel buffer, in 32-bit units)
    };
    struct Group {
        uint32_t first, count, material; // global vertex index, global material index
    };
    struct GpuBuffer {
        Vulkan::BufferHandle buffer;
        std::vector<uint8_t> shadow;
        size_t used = 0, capacity = 0, dirty_lo = SIZE_MAX, dirty_hi = 0;
        bool create(Vulkan::Device &device, size_t bytes);
        size_t append(const void *data, size_t bytes); // SIZE_MAX when full
        void write(size_t offset, const void *data, size_t bytes);
        void move(size_t from, size_t to, size_t bytes); // (within the CPU copy; marked for upload)
        void upload(Vulkan::CommandBuffer &cmd);
    };

    void dump_frame(const Recording &rec, const std::string &name);
    // the palettes as they were at the end of the recording `serial` (one of hist's, or the newest)
    void palettes_of(uint64_t serial);
    uint32_t upload_material(const ElRdMaterial &m);
    void write_material(uint32_t index, const ElRdMaterial &m);
    // resources_reset: keep what the recordings (finished, pending, being built) name, moved to the
    // buffers' start and renumbered in them; the rest is dropped
    void keep_recorded();
    // render(): does the item name resources that are there? (no: counted in life.stale, skipped)
    bool item_ok(const Recording &rec, const Recording::Item &it) const;
    void small_buffers();        // ELTALE_ND_SMALL_BUFFERS (a test)
    ElRdMeshDesc mesh_desc(uint32_t id, std::vector<ElRdGroup> &groups) const;
    static uint64_t mesh_hash(const ElRdMeshDesc &d);
    bool same_mesh(const ElRdMeshDesc &d, uint32_t id) const;
    void print_lifetime() const; // P1_ND_LIFETIME
    bool want_dump(uint32_t frame) const;
    // the depth, the decal's copy of it and (MSAA) the multisampled colour, for this size and sample count
    bool ensure_targets(uint32_t width, uint32_t height, uint32_t samples);

    Vulkan::Device *device = nullptr;
    Vulkan::Program *program = nullptr;
    GpuBuffer vertices, materials, texels, palettes;
    std::vector<Mesh> meshes;
    std::vector<Group> groups;
    std::vector<ElRdMaterial> material_cpu; // for the pipeline state
    std::vector<Texture> textures;          // the texel uploads, in the buffer's order (resources_reset)
    LifetimeStats life;
    // what the last full buffer kept, by content (an upload of the same is given it: resources_reset)
    std::unordered_multimap<uint64_t, Texture> kept_texture_index;
    std::unordered_multimap<uint64_t, uint32_t> kept_mesh_index, kept_rect_index;
    bool small = false; // ELTALE_ND_SMALL_BUFFERS
    uint32_t frame_base = 0, frame_last = 0; // recordings' numbers after a reset: the game's plus frame_base

    Recording building;
    std::deque<Recording> hist;    // the last few finished recordings, oldest first
    std::deque<Recording> pending; // built, waiting for their graphics task's number
    bool state_dirty = true, in_frame = false;
    uint64_t vi_now = 0, serial = 0;
    float phase = 0;
    float aspect_value = 4.0f / 3.0f;
    uint64_t palette_serial = 0; // whose palettes the palette buffer holds (0: the newest)
    std::unique_ptr<Interp> interp;
    uint64_t planned_a = 0, planned_b = 0; // the pair the interpolator has planned (serials)
    bool planned_ok = false;
    const char *planned_why = nullptr;
    PictureStats pstats;
    int dump_between = 0;          // ELTALE_DUMP_BETWEEN: pictures per tick in the dumps
    Recording::State state = {};
    float distance = 1.0f;
    bool distance_forced = false;

    Vulkan::ImageHandle depth;
    Vulkan::ImageHandle depth_before; // the depth before a decal draw (the RDP's two-sided decal test)
    Vulkan::ImageHandle msaa_color;   // the multisampled picture (opt.samples > 1), resolved into the picture
    uint32_t target_w = 0, target_h = 0, target_samples = 0;
    Vulkan::Program *program_ms = nullptr; // the same shaders, the decal reading a multisampled depth
    Vulkan::ImageHandle dump_picture;
    DrawOptions dump_opt;             // the dumps' look (the N64's; ELTALE_DUMP_* for the modern options)
    uint32_t dump_scale = 1;
    std::string dump_dir, dump_spec;
};

NativeDraw &native_draw();

} // namespace render
