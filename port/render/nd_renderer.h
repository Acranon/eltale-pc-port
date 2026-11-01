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
#include <vector>

namespace render {

struct DrawOptions {
    bool border = false;      // the N64's 8-pixel border: scissor 304x224 inside 320x240
    bool three_point = true;  // N64 three-point filtering (else four-point bilinear)
    bool n64_fog = false;     // the N64's fog factor, per vertex (else per pixel)
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

class NativeDraw {
public:
    bool init(Vulkan::Device &device);
    void shutdown();
    // the function table the game calls (one renderer per process)
    const ElRenderApi *api();

    // the newest finished recording (null before the first)
    const Recording *latest() const { return hist.empty() ? nullptr : &hist.back(); }
    // The picture for the time v, in retraces presented (set_vi's count, with the fraction since the
    // last): a recording is due BETWEEN_DELAY retraces after it was finished, and between two due
    // times the picture is the pair's picture between (nd_interp.h). `between` false: the newest as is.
    const Recording *picture(double v, bool between);
    void set_vi(uint64_t vi) { vi_now = vi; }
    const PictureStats &picture_stats() const { return pstats; }
    const InterpStats &interp_stats() const;
    void print_stats() const;
    static constexpr double BETWEEN_DELAY = 2.0;
    // Draw `rec` into `picture` (320x240 RGBA8, GENERAL layout), with its own depth buffer
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

private:
    struct Mesh {
        uint32_t first;  // vertices
        uint32_t group_first, group_count;
        float bounds[6];
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
        void upload(Vulkan::CommandBuffer &cmd);
    };

    void dump_frame(const Recording &rec, const std::string &name);
    // the palettes as they were at the end of the recording `serial` (one of hist's, or the newest)
    void palettes_of(uint64_t serial);
    uint32_t upload_material(const ElRdMaterial &m);
    bool want_dump(uint32_t frame) const;

    Vulkan::Device *device = nullptr;
    Vulkan::Program *program = nullptr;
    GpuBuffer vertices, materials, texels, palettes;
    std::vector<Mesh> meshes;
    std::vector<Group> groups;
    std::vector<ElRdMaterial> material_cpu; // for the pipeline state

    Recording building;
    std::deque<Recording> hist;    // the last few finished recordings, oldest first
    std::deque<Recording> pending; // built, waiting for their graphics task's number
    bool state_dirty = true, in_frame = false;
    uint64_t vi_now = 0, serial = 0;
    uint64_t palette_serial = 0; // whose palettes the palette buffer holds (0: the newest)
    std::unique_ptr<Interp> interp;
    uint64_t planned_a = 0, planned_b = 0; // the pair the interpolator has planned (serials)
    bool planned_ok = false;
    const char *planned_why = nullptr;
    PictureStats pstats;
    int dump_between = 0;          // ELTALE_DUMP_BETWEEN: pictures per tick in the dumps
    Recording::State state = {};

    Vulkan::ImageHandle depth;
    Vulkan::ImageHandle depth_before; // the depth before a decal draw (the RDP's two-sided decal test)
    Vulkan::ImageHandle dump_picture;
    std::string dump_dir, dump_spec;
};

NativeDraw &native_draw();

} // namespace render
