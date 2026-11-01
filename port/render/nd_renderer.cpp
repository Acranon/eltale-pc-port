// The native renderer's core (nd_renderer.h, docs/renderer/NATIVE_DRAW.md).
//
// Resources live in four GPU storage buffers that only grow (vertices, materials, texels, palettes),
// each with a CPU copy; what changed since the last picture is copied up before it is drawn. When one
// is full the loader resets them all and loads again (nd_model.c).
//
// A recording is drawn in its own order: per draw item, per group of its mesh, one GPU draw with the
// group's material. The pipeline state (depth test and write, decal bias, cull, the blend with the
// picture) comes from the material's othermode and geometry bits; everything else is the shaders'
// (shaders/nd.vert, shaders/nd.frag).
#include "nd_renderer.h"

#include "nd_interp.h"
#include "nd_shaders.h"
#include "png.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace render {

using namespace Vulkan;

namespace {

constexpr uint32_t W = 320, H = 240;
constexpr size_t VERTEX_CAP = size_t(1) << 21;   // 64 MiB of vertices
constexpr size_t MATERIAL_CAP = size_t(1) << 16;
constexpr size_t TEXEL_CAP = size_t(1) << 24;    // 64 MiB of texels
constexpr size_t PALETTE_CAP = 4096 * 256;

enum : uint32_t { OPT_THREE_POINT = 1, OPT_FAR_CLIP = 2, OPT_N64_FOG = 4, OPT_DECAL = 8, OPT_SKIP = 1u << 31 };

struct GpuTile {
    uint32_t texels, wh, fmt, mode;
    int32_t sl, tl, sh, th;
};
struct GpuMaterial {
    uint32_t combine0, combine1, othermode_h, othermode_l;
    uint32_t geometry, own, prim, env;
    uint32_t blend, fog, misc, flags;
    GpuTile tile[2];
};
static_assert(sizeof(GpuMaterial) == 112, "GpuMaterial");
struct GpuDraw {
    float mvp[16];
    float light_dir[3][4];
    float light_col[3][4];
    float ambient[4];
    float lookat_x[4], lookat_y[4];
    uint32_t colors[4];
    float fog[4];
    float tile_offset[4];
};
static_assert(sizeof(GpuDraw) == 256, "GpuDraw");
struct Push {
    uint32_t draw, material, options, pad;
};

uint32_t pack(const uint8_t *c) {
    return uint32_t(c[0]) | uint32_t(c[1]) << 8 | uint32_t(c[2]) << 16 | uint32_t(c[3]) << 24;
}

// c = a * b, row vectors (v * a * b), m[row * 4 + col]
void mul(const float *a, const float *b, float *c) {
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += double(a[i * 4 + k]) * b[k * 4 + j];
            r[i * 4 + j] = float(s);
        }
    std::memcpy(c, r, sizeof r);
}

// A world direction into a model's object space as the RSP does it (by the matrix's transpose), normalized
void to_object(const float *m, float x, float y, float z, float *out) {
    float v[3];
    for (int r = 0; r < 3; r++) v[r] = m[r * 4 + 0] * x + m[r * 4 + 1] * y + m[r * 4 + 2] * z;
    float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (int r = 0; r < 3; r++) out[r] = l > 0 ? v[r] / l : 0;
    out[3] = 0;
}

GpuTile gpu_tile(const ElRdTile &t) {
    GpuTile g = {};
    g.texels = t.texels;
    g.wh = uint32_t(t.width) | uint32_t(t.height) << 16;
    g.fmt = uint32_t(t.format) | uint32_t(t.palette) << 16;
    g.mode = uint32_t(t.cms) | uint32_t(t.cmt) << 2 | uint32_t(t.masks) << 4 | uint32_t(t.maskt) << 8 |
             uint32_t(t.shifts) << 12 | uint32_t(t.shiftt) << 16;
    g.sl = t.sl; g.tl = t.tl; g.sh = t.sh; g.th = t.th;
    return g;
}

// Z_MODE decal with the Z compare on: the RDP's two-sided test, done in the pixel stage (OPT_DECAL)
bool is_decal(const ElRdMaterial &m) {
    return (m.geometry & 1) && (m.othermode_l & 0x10) && (m.othermode_l >> 10 & 3) == 3;
}

bool writes_depth(const ElRdMaterial &m) { return (m.geometry & 1) && (m.othermode_l & 0x20); }

// The GPU pipeline state a material's othermode and geometry bits ask for
void apply_state(CommandBuffer &cmd, const ElRdMaterial &m) {
    const uint32_t ol = m.othermode_l, geom = m.geometry;
    const bool zbuf = geom & 1; // G_ZBUFFER
    const bool decal = is_decal(m);
    const bool zcmp = zbuf && (ol & 0x10) && !decal, zupd = zbuf && (ol & 0x20);
    cmd.set_depth_test(zcmp || zupd, zupd);
    cmd.set_depth_compare(zcmp ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_ALWAYS);
    VkCullModeFlags cull = 0;
    if (geom & 0x2000) cull |= VK_CULL_MODE_BACK_BIT;
    if (geom & 0x1000) cull |= VK_CULL_MODE_FRONT_BIT;
    cmd.set_cull_mode(cull);
    // the blender's last cycle with the picture (M = memory) under FORCE_BL: the GPU's blend
    const bool two = (m.othermode_h >> 20 & 3) == 1;
    const uint32_t mm = two ? ol >> 20 & 3 : ol >> 22 & 3, mb = two ? ol >> 16 & 3 : ol >> 18 & 3;
    if ((ol & 0x4000) && mm == 1) {
        cmd.set_blend_enable(true);
        const VkBlendFactor dst = mb == 0 ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : mb == 3 ? VK_BLEND_FACTOR_ZERO
                                                                                         : VK_BLEND_FACTOR_ONE;
        cmd.set_blend_factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ZERO, dst, VK_BLEND_FACTOR_ONE);
        cmd.set_blend_op(VK_BLEND_OP_ADD, VK_BLEND_OP_ADD);
    } else {
        cmd.set_blend_enable(false);
    }
}

NativeDraw instance;

// ---- the C function table -------------------------------------------------------------------------
uint32_t api_texture_upload(const uint32_t *t, uint32_t n) { return instance.texture_upload(t, n); }
uint32_t api_mesh_upload(const ElRdMeshDesc *m) { return instance.mesh_upload(m); }
void api_palette(uint32_t id, const uint16_t *e, uint32_t n) { instance.palette(id, e, n); }
void api_resources_reset() { instance.resources_reset(); }
void api_frame_begin(const ElRdFrameInfo *i) { instance.frame_begin(i); }
void api_camera(const ElRdCamera *c) { instance.camera(c); }
void api_lights(const ElRdLights *l) { instance.lights(l); }
void api_env(const uint8_t *c) { instance.env_color(c); }
void api_prim(const uint8_t *c) { instance.prim_color(c); }
void api_fog(const uint8_t *c) { instance.fog_color(c); }
void api_fog_position(int16_t m, int16_t o) { instance.fog_position(m, o); }
void api_draw(const ElRdDraw *d) { instance.draw(d); }
void api_frame_end(uint32_t f) { instance.frame_end(f); }
uint32_t api_material_2d(const ElRdMaterial *m) { return instance.material_2d(m); }
void api_draw_2d(const ElRd2D *d) { instance.draw_2d(d); }

const ElRenderApi api_table = {
    EL_RENDER_API_VERSION, api_texture_upload, api_mesh_upload, api_palette, api_resources_reset,
    api_frame_begin, api_camera, api_lights, api_env, api_prim, api_fog, api_fog_position, api_draw, api_frame_end,
    api_material_2d, api_draw_2d,
};

} // namespace

NativeDraw &native_draw() { return instance; }

const ElRenderApi *NativeDraw::api() { return &api_table; }

// ---- GPU buffers ------------------------------------------------------------------------------------

bool NativeDraw::GpuBuffer::create(Device &dev, size_t bytes) {
    BufferCreateInfo info = {};
    info.size = bytes;
    info.domain = BufferDomain::Device;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer = dev.create_buffer(info);
    capacity = bytes;
    used = 0;
    shadow.clear();
    return bool(buffer);
}

size_t NativeDraw::GpuBuffer::append(const void *data, size_t bytes) {
    if (used + bytes > capacity) return SIZE_MAX;
    const size_t at = used;
    write(at, data, bytes);
    used += bytes;
    return at;
}

void NativeDraw::GpuBuffer::write(size_t offset, const void *data, size_t bytes) {
    if (shadow.size() < offset + bytes) shadow.resize(std::max(offset + bytes, std::min(capacity, shadow.size() * 2)));
    std::memcpy(shadow.data() + offset, data, bytes);
    dirty_lo = std::min(dirty_lo, offset);
    dirty_hi = std::max(dirty_hi, offset + bytes);
}

void NativeDraw::GpuBuffer::upload(CommandBuffer &cmd) {
    if (dirty_lo >= dirty_hi) return;
    const size_t lo = dirty_lo & ~size_t(3), hi = (dirty_hi + 3) & ~size_t(3);
    void *p = cmd.update_buffer(*buffer, lo, hi - lo);
    std::memcpy(p, shadow.data() + lo, hi - lo);
    dirty_lo = SIZE_MAX;
    dirty_hi = 0;
}

// ---- set-up ----------------------------------------------------------------------------------------

bool NativeDraw::init(Device &dev) {
    device = &dev;
    if (!interp) interp.reset(new Interp());
    if (const char *e = std::getenv("ELTALE_DUMP_BETWEEN")) dump_between = std::atoi(e);
    ResourceLayout vert = {};
    vert.push_constant_size = sizeof(Push);
    vert.sets[0].storage_buffer_mask = 1u << 0 | 1u << 1;
    ResourceLayout frag = {};
    frag.output_mask = 1;
    frag.push_constant_size = sizeof(Push);
    frag.sets[0].storage_buffer_mask = 1u << 1 | 1u << 2 | 1u << 3 | 1u << 4;
    frag.sets[0].sampled_image_mask = 1u << 5;
    for (int b = 0; b < 6; b++) {
        vert.sets[0].meta[b].array_size = 1;
        frag.sets[0].meta[b].array_size = 1;
    }
    Shader *vs = dev.request_shader(nd_spv_nd_vert, sizeof(nd_spv_nd_vert), &vert);
    Shader *fs = dev.request_shader(nd_spv_nd_frag, sizeof(nd_spv_nd_frag), &frag);
    program = dev.request_program(vs, fs);
    if (!program) return false;
    if (!vertices.create(dev, VERTEX_CAP * sizeof(ElRdVertex)) || !materials.create(dev, MATERIAL_CAP * sizeof(GpuMaterial)) ||
        !texels.create(dev, TEXEL_CAP * 4) || !palettes.create(dev, PALETTE_CAP * 4))
        return false;
    palettes.write(0, std::vector<uint32_t>(PALETTE_CAP).data(), PALETTE_CAP * 4);
    palettes.used = PALETTE_CAP * 4;
    // the depth buffer, and its copy for decals (both GENERAL: a render pass, a copy and sampling all
    // use them without layout changes)
    ImageCreateInfo zi = ImageCreateInfo::render_target(W, H, VK_FORMAT_D32_SFLOAT);
    zi.layout = ImageLayout::General;
    zi.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
    depth = dev.create_image(zi);
    ImageCreateInfo ci = zi;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    depth_before = dev.create_image(ci);
    return bool(depth) && bool(depth_before);
}

void NativeDraw::shutdown() {
    if (interp && (interp->stats.plans || pstats.newest || pstats.between)) print_stats();
    depth.reset();
    depth_before.reset();
    dump_picture.reset();
    vertices.buffer.reset();
    materials.buffer.reset();
    texels.buffer.reset();
    palettes.buffer.reset();
    device = nullptr;
}

void NativeDraw::set_dump(const std::string &dir, const std::string &spec) {
    dump_dir = dir;
    dump_spec = spec;
}

// spec: comma-separated "N", "A-B" (a range), "F+S" (every S-th from F), "F+S/R" (runs of R frames every S-th)
bool NativeDraw::want_dump(uint32_t frame) const {
    if (dump_dir.empty()) return false;
    if (dump_spec.empty()) return true;
    for (const char *p = dump_spec.c_str(); p && *p; p = std::strchr(p, ',') ? std::strchr(p, ',') + 1 : nullptr) {
        char *e = nullptr;
        const unsigned long first = std::strtoul(p, &e, 10);
        if (*e == '-') {
            const unsigned long last = std::strtoul(e + 1, nullptr, 10);
            if (frame >= first && frame <= last) return true;
        } else if (*e == '+') {
            char *e2 = nullptr;
            const unsigned long step = std::strtoul(e + 1, &e2, 10);
            const unsigned long run = *e2 == '/' ? std::strtoul(e2 + 1, nullptr, 10) : 1;
            if (frame >= first && step && (frame - first) % step < run) return true;
        } else if (frame == first) {
            return true;
        }
    }
    return false;
}

// ---- resources ----------------------------------------------------------------------------------------

uint32_t NativeDraw::texture_upload(const uint32_t *t, uint32_t count) {
    const size_t at = texels.append(t, size_t(count) * 4);
    return at == SIZE_MAX ? ~0u : uint32_t(at / 4);
}

uint32_t NativeDraw::mesh_upload(const ElRdMeshDesc *d) {
    if (vertices.used + size_t(d->vertex_count) * sizeof(ElRdVertex) > vertices.capacity ||
        materials.used + size_t(d->material_count) * sizeof(GpuMaterial) > materials.capacity)
        return ~0u;
    Mesh m = {};
    m.first = uint32_t(vertices.append(d->vertices, size_t(d->vertex_count) * sizeof(ElRdVertex)) / sizeof(ElRdVertex));
    const uint32_t mat0 = uint32_t(material_cpu.size());
    for (uint32_t i = 0; i < d->material_count; i++) upload_material(d->materials[i]);
    m.group_first = uint32_t(groups.size());
    m.group_count = d->group_count;
    for (uint32_t i = 0; i < d->group_count; i++)
        groups.push_back({ m.first + d->groups[i].first, d->groups[i].count, mat0 + d->groups[i].material });
    std::memcpy(m.bounds, d->bounds, sizeof m.bounds);
    meshes.push_back(m);
    return uint32_t(meshes.size() - 1);
}

// one material into the materials buffer (the caller checked the room) -> its index
uint32_t NativeDraw::upload_material(const ElRdMaterial &s) {
    GpuMaterial g = {};
    g.combine0 = s.combine[0];
    g.combine1 = s.combine[1];
    g.othermode_h = s.othermode_h;
    g.othermode_l = s.othermode_l;
    g.geometry = s.geometry;
    g.own = s.own;
    g.prim = pack(s.prim);
    g.env = pack(s.env);
    g.blend = pack(s.blend);
    g.fog = pack(s.fog);
    g.misc = uint32_t(s.prim_lod_frac) | uint32_t(s.k4) << 8 | uint32_t(s.k5) << 16;
    g.flags = s.flags;
    g.tile[0] = gpu_tile(s.tile[0]);
    g.tile[1] = gpu_tile(s.tile[1]);
    materials.append(&g, sizeof g);
    material_cpu.push_back(s);
    return uint32_t(material_cpu.size() - 1);
}

uint32_t NativeDraw::material_2d(const ElRdMaterial *m) {
    if (materials.used + sizeof(GpuMaterial) > materials.capacity) return ~0u;
    return upload_material(*m);
}

void NativeDraw::palette(uint32_t id, const uint16_t *e, uint32_t count) {
    if (size_t(id + 1) * 256 > PALETTE_CAP || count > 256) return;
    palettes_of(0); // (changes go on top of the newest)
    uint32_t v[256];
    for (uint32_t i = 0; i < count; i++) v[i] = e[i];
    const size_t at = size_t(id) * 256 * 4;
    // (outside a tick: a change to the newest recording's palettes)
    Recording *owner = in_frame ? &building : hist.empty() ? nullptr : &hist.back();
    if (owner) {
        Recording::PaletteChange c;
        c.id = id;
        c.count = count;
        std::memcpy(c.before, palettes.shadow.data() + at, count * 4);
        std::memcpy(c.after, v, count * 4);
        owner->palettes.push_back(c);
    }
    palettes.write(at, v, count * 4);
}

// Puts the palette buffer back to how it was at the end of recording `serial` (0: the newest): the
// changes of the recordings after it undone, newest first, or redone
void NativeDraw::palettes_of(uint64_t want) {
    if (!hist.empty() && want == hist.back().serial) want = 0;
    if (want == palette_serial) return;
    // redo up to the newest
    if (palette_serial != 0) {
        for (const Recording &r : hist)
            if (r.serial > palette_serial)
                for (const Recording::PaletteChange &c : r.palettes) palettes.write(size_t(c.id) * 256 * 4, c.after, c.count * 4);
        palette_serial = 0;
    }
    if (want == 0) return;
    bool known = false;
    for (const Recording &r : hist) known |= r.serial == want;
    if (!known) return; // (gone: the newest's)
    for (auto r = hist.rbegin(); r != hist.rend() && r->serial > want; ++r)
        for (auto c = r->palettes.rbegin(); c != r->palettes.rend(); ++c)
            palettes.write(size_t(c->id) * 256 * 4, c->before, c->count * 4);
    palette_serial = want;
}

void NativeDraw::resources_reset() {
    if (device) device->wait_idle();
    vertices.used = materials.used = texels.used = 0;
    meshes.clear();
    groups.clear();
    material_cpu.clear();
}

// ---- recording -----------------------------------------------------------------------------------

void NativeDraw::frame_begin(const ElRdFrameInfo *info) {
    // The game can build the next frame before the last one's graphics task starts (the menus, the
    // title: they wait only for the swap): the finished one waits for its number (frame_end)
    if (in_frame) pending.push_back(std::move(building));
    while (pending.size() > 4) pending.pop_front(); // (a build no task took)
    building = Recording();
    building.info = *info;
    building.items.clear();
    building.items2d.clear();
    building.states.clear();
    building.has_camera = false;
    state_dirty = true;
    in_frame = true;
}

void NativeDraw::camera(const ElRdCamera *c) {
    building.camera = *c;
    building.has_camera = true;
}

void NativeDraw::lights(const ElRdLights *l) { state.lights = *l; state_dirty = true; }
void NativeDraw::env_color(const uint8_t *c) { std::memcpy(state.env, c, 4); state_dirty = true; }
void NativeDraw::prim_color(const uint8_t *c) { std::memcpy(state.prim, c, 4); state_dirty = true; }
void NativeDraw::fog_color(const uint8_t *c) { std::memcpy(state.fog, c, 4); state_dirty = true; }
void NativeDraw::fog_position(int16_t mul_, int16_t off) { state.fog_mul = mul_; state.fog_off = off; state_dirty = true; }

void NativeDraw::draw(const ElRdDraw *d) {
    if (!in_frame || d->mesh >= meshes.size()) return;
    if (state_dirty || building.states.empty()) {
        building.states.push_back(state);
        state_dirty = false;
    }
    building.items.push_back({ *d, uint32_t(building.states.size() - 1) });
}

void NativeDraw::draw_2d(const ElRd2D *d) {
    if (!in_frame) return;
    if (d->mesh == EL_RD2D_RECT ? d->material >= material_cpu.size() : d->mesh >= meshes.size()) return;
    if (state_dirty || building.states.empty()) {
        building.states.push_back(state);
        state_dirty = false;
    }
    Recording::Item it = {};
    it.state = uint32_t(building.states.size() - 1);
    it.item2d = int32_t(building.items2d.size());
    building.items2d.push_back(*d);
    building.items.push_back(it);
}

void NativeDraw::frame_end(uint32_t frame) {
    Recording r;
    if (!pending.empty()) {
        r = std::move(pending.front());
        pending.pop_front();
    } else {
        if (!in_frame) return;
        in_frame = false;
        r = std::move(building);
    }
    r.frame = frame;
    r.vi = vi_now;
    r.serial = ++serial;
    hist.push_back(std::move(r));
    while (hist.size() > 3) hist.pop_front();
    if (!want_dump(frame)) return;
    const Recording &cur = hist.back();
    // ELTALE_DUMP_BETWEEN=N: the pictures between the last tick and this one, at j/N (frame_<last>_<j*1000/N>);
    // ELTALE_INTERP_ENDS=1: also the picture at t = 0 through the interpolator (ends_<last>_0.png, which
    // tools/interp_ends.py checks against the tick's own) and P1_INTERP_ENDS: the matrices and camera that
    // differ bit for bit at t = 0 from the last tick's and at t = 1 from this one's
    if (dump_between > 1 && hist.size() >= 2) {
        const Recording &prev = hist[hist.size() - 2];
        const char *why = nullptr;
        const bool ok = interp->plan(prev, cur, hist.size() >= 3 ? &hist[hist.size() - 3] : nullptr, &why);
        planned_a = planned_b = 0; // (the window's plan is gone)
        static const bool ends = std::getenv("ELTALE_INTERP_ENDS") != nullptr;
        if (ends && ok) {
            int at0, at1;
            interp->check_ends(&at0, &at1);
            std::printf("P1_INTERP_ENDS frame=%u differ_at_0=%d differ_at_1=%d\n", prev.frame, at0, at1);
            dump_frame(interp->build(0.0f), "ends_" + std::to_string(prev.frame) + "_0");
        }
        for (int j = 1; j < dump_between; j++) {
            char name[64];
            std::snprintf(name, sizeof name, "frame_%u_%03d", prev.frame, j * 1000 / dump_between);
            dump_frame(ok ? interp->build(float(j) / float(dump_between)) : prev, name);
        }
        if (!ok) std::printf("P1_INTERP_SNAP frame=%u why=%s\n", prev.frame, why ? why : "?");
    }
    dump_frame(cur, "frame_" + std::to_string(frame));
}

const Recording *NativeDraw::picture(double v, bool between) {
    if (hist.empty()) return nullptr;
    if (!between) return &hist.back();
    // the newest recording due by v
    int k = -1;
    for (int i = int(hist.size()) - 1; i >= 0; i--)
        if (double(hist[size_t(i)].vi) + BETWEEN_DELAY <= v) { k = i; break; }
    if (k < 0) { // none due yet: the oldest kept (a start, after a reset)
        pstats.fallback++;
        pstats.early++;
        return &hist.front();
    }
    if (size_t(k) + 1 == hist.size()) { // nothing newer yet: the newest as it is
        pstats.newest++;
        return &hist.back();
    }
    const Recording &a = hist[size_t(k)], &b = hist[size_t(k) + 1];
    const double span = double(b.vi) - double(a.vi), t = (v - (double(a.vi) + BETWEEN_DELAY)) / span;
    if (t <= 0) { pstats.newest++; return &a; }
    if (span > 4) { pstats.fallback++; pstats.gap++; return &a; } // a pause, a load: no picture between
    if (planned_a != a.serial || planned_b != b.serial) {
        planned_a = a.serial;
        planned_b = b.serial;
        planned_ok = interp->plan(a, b, k > 0 ? &hist[size_t(k) - 1] : nullptr, &planned_why);
    }
    if (!planned_ok) {
        pstats.fallback++;
        if (planned_why && !std::strcmp(planned_why, "cut")) pstats.cut++;
        else if (planned_why && !std::strcmp(planned_why, "mode")) pstats.mode++;
        else pstats.camera++;
        return &a;
    }
    pstats.between++;
    return &interp->build(float(t < 1 ? t : 1));
}

const InterpStats &NativeDraw::interp_stats() const { return interp->stats; }

void NativeDraw::print_stats() const {
    const InterpStats &s = interp->stats;
    std::printf("P1_INTERP pictures=%llu between=%llu newest=%llu fallback=%llu (early %llu gap %llu mode %llu camera %llu cut %llu) "
                "plans=%llu items=%llu moving=%llu unmatched=%llu snapped=%llu plan_us=%.1f/%.1f build_us=%.1f/%.1f\n",
                (unsigned long long)(pstats.between + pstats.newest + pstats.fallback), (unsigned long long)pstats.between,
                (unsigned long long)pstats.newest, (unsigned long long)pstats.fallback, (unsigned long long)pstats.early,
                (unsigned long long)pstats.gap, (unsigned long long)pstats.mode, (unsigned long long)pstats.camera,
                (unsigned long long)pstats.cut, (unsigned long long)s.plans, (unsigned long long)s.items,
                (unsigned long long)s.moving, (unsigned long long)s.unmatched, (unsigned long long)s.snapped,
                s.plans ? s.plan_us / double(s.plans) : 0.0, s.plan_max_us, s.pictures ? s.build_us / double(s.pictures) : 0.0,
                s.build_max_us);
    std::fflush(stdout);
}

// ---- drawing --------------------------------------------------------------------------------------

void NativeDraw::render(CommandBuffer &cmd, Image &picture, const Recording *rec, const DrawOptions &opt) {
    if (rec) palettes_of(rec->serial); // (an older recording: its own palettes, as they were)
    vertices.upload(cmd);
    materials.upload(cmd);
    texels.upload(cmd);
    palettes.upload(cmd);
    cmd.barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    // the draws' constants
    std::vector<GpuDraw> gd;
    std::vector<uint32_t> options;
    const bool camera_ok = rec && rec->has_camera;
    // 2D rectangles: 6 vertices each, in screen units (a buffer of this picture's own)
    std::vector<ElRdVertex> rect_vertices;
    std::vector<uint32_t> rect_first;
    if (rec && !rec->items.empty()) {
        gd.resize(rec->items.size());
        options.resize(rec->items.size());
        const ElRdCamera &cam = rec->camera;
        for (size_t i = 0; i < rec->items.size(); i++) {
            const Recording::Item &it = rec->items[i];
            const Recording::State &s = rec->states[it.state];
            GpuDraw &g = gd[i];
            std::memset(&g, 0, sizeof g);
            if (it.item2d >= 0) {
                const ElRd2D &d = rec->items2d[size_t(it.item2d)];
                if (d.mesh == EL_RD2D_RECT) {
                    // pixels to clip space (x right, y down; the vertex stage flips y back)
                    static const float screen[16] = { 1.0f / 160, 0, 0, 0, 0, -1.0f / 120, 0, 0, 0, 0, 0, 0, -1, 1, 0, 1 };
                    std::memcpy(g.mvp, screen, sizeof screen);
                    // the RDP's texel at pixel (x, y): s0 + (x - x0) * dsdx; a little over, so a texel
                    // boundary the interpolation lands just under still floors to the right texel
                    const float eps = 1.0f / 256, x0 = d.rect[0], y0 = d.rect[1], x1 = d.rect[2], y1 = d.rect[3];
                    auto vert = [&](float x, float y) {
                        ElRdVertex v = {};
                        v.x = x;
                        v.y = y;
                        v.s = d.st[0] + (x - x0) * d.st[2] + eps;
                        v.t = d.st[1] + (y - y0) * d.st[3] + eps;
                        rect_vertices.push_back(v);
                    };
                    rect_first.resize(i + 1, 0);
                    rect_first[i] = uint32_t(rect_vertices.size());
                    vert(x0, y0); vert(x1, y0); vert(x1, y1);
                    vert(x0, y0); vert(x1, y1); vert(x0, y1);
                } else {
                    std::memcpy(g.mvp, d.matrix, sizeof g.mvp);
                }
                g.colors[0] = pack(d.env);
                g.colors[1] = pack(d.prim);
                g.colors[2] = pack(d.fog);
                g.colors[3] = pack(d.blend);
                g.fog[2] = 0.0f; // the depth written (no depth test for 2D)
                g.fog[3] = 1.0f;
                options[i] = opt.three_point ? OPT_THREE_POINT : 0;
                continue;
            }
            if (!camera_ok && !(it.draw.flags & EL_RDD_SCREEN)) {
                options[i] = OPT_SKIP;
                continue;
            }
            if (it.draw.flags & EL_RDD_SCREEN) std::memcpy(g.mvp, it.draw.matrix, sizeof g.mvp);
            else mul(it.draw.matrix, cam.projection, g.mvp);
            for (int l = 0; l < 3; l++) {
                to_object(it.draw.matrix, s.lights.dir[l][0], s.lights.dir[l][1], s.lights.dir[l][2], g.light_dir[l]);
                for (int c = 0; c < 3; c++) g.light_col[l][c] = s.lights.color[l][c] / 255.0f;
            }
            for (int c = 0; c < 3; c++) g.ambient[c] = s.lights.ambient[c] / 255.0f;
            to_object(it.draw.matrix, cam.lookat_x[0], cam.lookat_x[1], cam.lookat_x[2], g.lookat_x);
            to_object(it.draw.matrix, cam.lookat_y[0], cam.lookat_y[1], cam.lookat_y[2], g.lookat_y);
            g.colors[0] = pack(s.env);
            g.colors[1] = pack(s.prim);
            g.colors[2] = pack(s.fog);
            g.colors[3] = pack(s.blend);
            g.fog[0] = s.fog_mul;
            g.fog[1] = s.fog_off;
            g.fog[2] = cam.near_plane > 0 ? cam.near_plane : 1.0f;
            // N64 screen-z units per unit of depth (near/w): the viewport's z scale (511, then 32 per
            // unit in the RDP) times the perspective's z/w per 1/w (its m[3][2]), over the near plane
            g.fog[3] = 511.0f * 32.0f * std::fabs(cam.perspective[14]) / g.fog[2];
            // a screen draw (no Z: the fade, the letterbox) has w = 1: depth 0 keeps it inside the clip volume
            if (it.draw.flags & EL_RDD_SCREEN) g.fog[2] = 0.0f;
            g.tile_offset[0] = it.draw.tile_offset[0];
            g.tile_offset[1] = it.draw.tile_offset[1];
            // does the mesh's box reach past the far plane? (then the pixel stage cuts there)
            uint32_t o = (opt.three_point ? OPT_THREE_POINT : 0) | (opt.n64_fog ? OPT_N64_FOG : 0);
            const Mesh &mesh = meshes[it.draw.mesh];
            for (int k = 0; k < 8 && !(o & OPT_FAR_CLIP); k++) {
                const float p[3] = { mesh.bounds[(k & 1) ? 3 : 0], mesh.bounds[(k & 2) ? 4 : 1], mesh.bounds[(k & 4) ? 5 : 2] };
                const float z = p[0] * g.mvp[2] + p[1] * g.mvp[6] + p[2] * g.mvp[10] + g.mvp[14];
                const float w = p[0] * g.mvp[3] + p[1] * g.mvp[7] + p[2] * g.mvp[11] + g.mvp[15];
                if (z > w) o |= OPT_FAR_CLIP;
            }
            options[i] = o;
        }
    }
    BufferHandle rect_buffer;
    if (!rect_vertices.empty()) {
        BufferCreateInfo info = {};
        info.size = rect_vertices.size() * sizeof(ElRdVertex);
        info.domain = BufferDomain::Device;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        rect_buffer = device->create_buffer(info, rect_vertices.data());
    }
    // ELTALE_ND_VIEW=n (a test): 1 the shade, 2 TEXEL0, 3 the shade alpha, 4 the combiner's output
    static const uint32_t view = (std::getenv("ELTALE_ND_VIEW") ? uint32_t(std::atoi(std::getenv("ELTALE_ND_VIEW"))) : 0);
    if (std::getenv("ELTALE_ND_DEBUG") && rec) {
        std::fprintf(stderr, "[nd] frame %u: camera %d items %zu states %zu\n", rec->frame, rec->has_camera, rec->items.size(),
                     rec->states.size());
        if (!gd.empty() && rec->items[0].item2d < 0 && !(options[0] & OPT_SKIP)) {
            const float *m = gd[0].mvp;
            const Mesh &mesh = meshes[rec->items[0].draw.mesh];
            const ElRdVertex *v = reinterpret_cast<const ElRdVertex *>(vertices.shadow.data()) + mesh.first;
            float c[4];
            for (int j = 0; j < 4; j++) c[j] = v->x * m[j] + v->y * m[4 + j] + v->z * m[8 + j] + m[12 + j];
            std::fprintf(stderr, "[nd] item 0 mesh %u (%u groups) v0 (%g %g %g) -> clip (%g %g %g %g) proj[0]=%g model[0]=%g\n",
                         rec->items[0].draw.mesh, mesh.group_count, v->x, v->y, v->z, c[0], c[1], c[2], c[3],
                         rec->camera.projection[0], rec->items[0].draw.matrix[0]);
        }
    }
    BufferHandle draw_buffer;
    if (!gd.empty()) {
        BufferCreateInfo info = {};
        info.size = gd.size() * sizeof(GpuDraw);
        info.domain = BufferDomain::Device;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        draw_buffer = device->create_buffer(info, gd.data());
    }

    const uint8_t *clear = rec ? rec->info.clear : nullptr;
    const VkClearColorValue clear_colour = { { clear ? clear[0] / 255.0f : 0, clear ? clear[1] / 255.0f : 0,
                                               clear ? clear[2] / 255.0f : 0, 1.0f } };
    RenderPassInfo rp;
    rp.num_color_attachments = 1;
    rp.color_attachments[0] = &picture.get_view();
    rp.depth_stencil = &depth->get_view();
    rp.clear_attachments = 1;
    rp.store_attachments = 1;
    rp.op_flags = RENDER_PASS_OP_CLEAR_DEPTH_STENCIL_BIT | RENDER_PASS_OP_STORE_DEPTH_STENCIL_BIT;
    rp.clear_depth_stencil.depth = 0.0f; // reversed Z
    rp.clear_color[0] = opt.border ? VkClearColorValue{ { 0, 0, 0, 1 } } : clear_colour;
    cmd.begin_render_pass(rp);
    VkRect2D inner = { { 8, 8 }, { W - 16, H - 16 } };
    if (opt.border) {
        VkClearValue v = {};
        v.color = clear_colour;
        cmd.clear_quad(0, { inner, 0, 1 }, v, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    // the state a render pass starts with (again after a decal's depth copy)
    auto setup = [&]() {
        cmd.set_opaque_state();
        cmd.set_program(program);
        cmd.set_storage_buffer(0, 0, *vertices.buffer);
        cmd.set_storage_buffer(0, 1, *draw_buffer);
        cmd.set_storage_buffer(0, 2, *materials.buffer);
        cmd.set_storage_buffer(0, 3, *texels.buffer);
        cmd.set_storage_buffer(0, 4, *palettes.buffer);
        cmd.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        cmd.set_front_face(VK_FRONT_FACE_COUNTER_CLOCKWISE);
        // the RDP samples a pixel at its top-left corner, the GPU at its centre: the picture moves half a pixel
        cmd.set_viewport({ 0.5f, 0.5f, float(W), float(H), 0, 1 });
        cmd.set_scissor(opt.border ? inner : VkRect2D{ { 0, 0 }, { W, H } });
        cmd.set_texture(0, 5, depth_before->get_view(), StockSampler::NearestClamp);
    };
    // A decal reads the depth as it was before it (the RDP's test is two-sided: depth_before): copied
    // when the first decal comes, and again when depth was written since.
    auto copy_depth = [&]() {
        cmd.end_render_pass();
        cmd.image_barrier(*depth, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_TRANSFER_READ_BIT);
        cmd.image_barrier(*depth_before, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT);
        cmd.copy_image(*depth_before, *depth);
        cmd.image_barrier(picture, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        cmd.image_barrier(*depth, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        cmd.image_barrier(*depth_before, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        RenderPassInfo again = rp;
        again.clear_attachments = 0;
        again.load_attachments = 1;
        again.op_flags = RENDER_PASS_OP_LOAD_DEPTH_STENCIL_BIT | RENDER_PASS_OP_STORE_DEPTH_STENCIL_BIT;
        cmd.begin_render_pass(again);
        setup();
    };
    if (!gd.empty()) {
        setup();
        bool copied = false, written = false;
        bool rect_bound = false;
        for (size_t i = 0; i < rec->items.size(); i++) {
            if (options[i] & OPT_SKIP) continue;
            const Recording::Item &item = rec->items[i];
            const ElRd2D *d2 = item.item2d >= 0 ? &rec->items2d[size_t(item.item2d)] : nullptr;
            if (d2 && d2->mesh == EL_RD2D_RECT) {
                if (!rect_bound) cmd.set_storage_buffer(0, 0, *rect_buffer);
                rect_bound = true;
                const ElRdMaterial &mat = material_cpu[d2->material];
                apply_state(cmd, mat);
                Push push = { uint32_t(i), d2->material, options[i], view };
                cmd.push_constants(&push, 0, sizeof push);
                cmd.draw(6, 1, rect_first[i], 0);
                continue;
            }
            if (rect_bound) cmd.set_storage_buffer(0, 0, *vertices.buffer);
            rect_bound = false;
            const Mesh &mesh = meshes[d2 ? d2->mesh : item.draw.mesh];
            for (uint32_t k = 0; k < mesh.group_count; k++) {
                const Group &grp = groups[mesh.group_first + k];
                const ElRdMaterial &mat = material_cpu[grp.material];
                const bool decal = is_decal(mat);
                if (decal && (!copied || written)) {
                    copy_depth();
                    rect_bound = false;
                    copied = true;
                    written = false;
                }
                if (writes_depth(mat)) written = true;
                apply_state(cmd, mat);
                Push push = { uint32_t(i), grp.material, options[i] | (decal ? OPT_DECAL : 0u), view };
                cmd.push_constants(&push, 0, sizeof push);
                cmd.draw(grp.count, 1, grp.first, 0);
            }
        }
    }
    cmd.end_render_pass();
}

void NativeDraw::dump_frame(const Recording &rec, const std::string &name) {
    if (!device) return;
    if (!dump_picture) {
        ImageCreateInfo info = ImageCreateInfo::render_target(W, H, VK_FORMAT_R8G8B8A8_UNORM);
        info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        dump_picture = device->create_image(info);
    }
    DrawOptions opt;
    opt.border = true;      // the N64 look: the border, three-point filtering, the N64's fog
    opt.three_point = true;
    opt.n64_fog = true;
    CommandBufferHandle cmd = device->request_command_buffer();
    cmd->full_barrier();
    render(*cmd, *dump_picture, &rec, opt);
    BufferCreateInfo bi = {};
    bi.size = W * H * 4;
    bi.domain = BufferDomain::CachedHost;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    BufferHandle buffer = device->create_buffer(bi);
    cmd->barrier(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    cmd->copy_image_to_buffer(*buffer, *dump_picture, 0, { 0, 0, 0 }, { W, H, 1 }, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });
    cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                 VK_ACCESS_2_HOST_READ_BIT);
    Fence fence;
    device->submit(cmd, &fence);
    fence->wait();
    const uint32_t *px = static_cast<const uint32_t *>(device->map_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT));
    std::vector<uint32_t> rgba(px, px + W * H);
    device->unmap_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT);
    for (uint32_t &p : rgba) p |= 0xFF000000u;
    const std::string path = dump_dir + "/" + name + ".png";
    if (!render_png_write(path.c_str(), rgba.data(), W, H)) std::fprintf(stderr, "[render] cannot write %s\n", path.c_str());
    if (name.compare(0, 6, "frame_") == 0 && name.find('_', 6) == std::string::npos)
        std::printf("P1_NATIVE_FRAME frame=%u items=%zu\n", rec.frame, rec.items.size());
    std::fflush(stdout);
    device->next_frame_context();
}

} // namespace render
