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
//
// Lifetimes (NATIVE_DRAW.md "Resets and full buffers"): a full buffer (resources_reset) keeps what the
// recordings name, renumbered, so items recorded before it stay good; the game's Reset (reset) drops the
// recordings with the resources. render() checks each item's ids and skips (and counts) one that is stale.
#include "nd_renderer.h"

#include "nd_interp.h"
#include "nd_shaders.h"
#include "png.h"

#include <algorithm>
#include <cassert>
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

enum : uint32_t { OPT_THREE_POINT = 1, OPT_FAR_CLIP = 2, OPT_N64_FOG = 4, OPT_DECAL = 8, OPT_DITHER = 16, OPT_RECT = 32, OPT_SKIP = 1u << 31 };

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

uint64_t fnv(const void *p, size_t n, uint64_t seed = 0) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    uint64_t h = UINT64_C(0xCBF29CE484222325) ^ seed;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * UINT64_C(0x100000001B3);
    return h;
}

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

// The eye in the world from a view matrix (row vectors: p_view = p R + T, so the eye is -T R^-1)
void overlay_eye(const float *v, float *out) {
    const double a = v[0], b = v[1], c = v[2], d = v[4], e = v[5], f = v[6], g = v[8], h = v[9], k = v[10];
    const double det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
    out[0] = out[1] = out[2] = out[3] = 0;
    if (std::fabs(det) < 1e-12) return;
    // R^-1 (the adjugate over the determinant), then -T R^-1
    const double inv[9] = { (e * k - f * h) / det, (c * h - b * k) / det, (b * f - c * e) / det,
                            (f * g - d * k) / det, (a * k - c * g) / det, (c * d - a * f) / det,
                            (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det };
    for (int j = 0; j < 3; j++) out[j] = float(-(v[12] * inv[j] + v[13] * inv[3 + j] + v[14] * inv[6 + j]));
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

// A cut-out: CVG x alpha or the alpha compare (the pixel stage discards by the texture's alpha). With MSAA
// such a draw is shaded at every sample (the discard per sample: smooth edges, as KF4's cut-out AA)
bool is_cutout(const ElRdMaterial &m) { return (m.othermode_l & 0x1000) || (m.othermode_l & 3) == 1; }

// The GPU pipeline state a material's othermode and geometry bits ask for (sample_shading: a 3D cut-out
// drawn multisampled)
void apply_state(CommandBuffer &cmd, const ElRdMaterial &m, bool sample_shading = false) {
    cmd.set_multisample_state(false, false, sample_shading);
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
void api_reset() { instance.reset(); }
void api_overlay(const ElRdVertex *v, uint32_t n, uint32_t m) { instance.overlay(v, n, m); }
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
void api_frame_phase(float p) { instance.frame_phase(p); }
float api_draw_distance() { return instance.draw_distance(); }
float api_aspect() { return instance.aspect(); }

const ElRenderApi api_table = {
    EL_RENDER_API_VERSION, api_texture_upload, api_mesh_upload, api_palette, api_resources_reset,
    api_frame_begin, api_camera, api_lights, api_env, api_prim, api_fog, api_fog_position, api_draw, api_frame_end,
    api_material_2d, api_draw_2d, api_frame_phase, api_draw_distance, api_aspect, api_reset, api_overlay,
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

void NativeDraw::GpuBuffer::move(size_t from, size_t to, size_t bytes) {
    if (from == to || !bytes) return;
    std::memmove(shadow.data() + to, shadow.data() + from, bytes);
    dirty_lo = std::min(dirty_lo, to);
    dirty_hi = std::max(dirty_hi, to + bytes);
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
    // (MSAA: the decal's depth copy is multisampled, read as such)
    Shader *fs_ms = dev.request_shader(nd_spv_nd_ms_frag, sizeof(nd_spv_nd_ms_frag), &frag);
    program_ms = dev.request_program(vs, fs_ms);
    // the headless dumps' look: the N64's (border, three-point, its fog) at 1x, unless ELTALE_DUMP_* ask for the
    // modern options (contact sheets): ELTALE_DUMP_SCALE=n, ELTALE_DUMP_MSAA=n, ELTALE_DUMP_LOOK=modern (no
    // border, fog per pixel), ELTALE_DUMP_LOOK=classic (the Classic preset's look: the N64 dither too),
    // ELTALE_DRAW_DISTANCE=k (the fog and far plane k times as far)
    dump_opt.border = dump_opt.three_point = dump_opt.n64_fog = true;
    if (const char *e = std::getenv("ELTALE_DUMP_LOOK")) {
        if (!std::strcmp(e, "modern")) dump_opt.border = dump_opt.n64_fog = false;
        if (!std::strcmp(e, "classic")) dump_opt.n64_dither = true;
    }
    if (const char *e = std::getenv("ELTALE_DUMP_SCALE")) dump_scale = uint32_t(std::min(std::max(std::atoi(e), 1), 8));
    // ELTALE_DUMP_PAIR=1 (tools/frame_test.py --scale): each dump also as the 1x picture without MSAA, which the
    // bigger one's 2D must equal in whole blocks (render_compare.py --blocks)
    dump_pair = std::getenv("ELTALE_DUMP_PAIR") && std::atoi(std::getenv("ELTALE_DUMP_PAIR")) != 0;
    if (const char *e = std::getenv("ELTALE_DUMP_MSAA")) {
        const int n = std::atoi(e);
        dump_opt.samples = n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 1;
    }
    if (const char *e = std::getenv("ELTALE_DRAW_DISTANCE")) {
        dump_opt.draw_distance = float(std::max(std::atof(e), 1.0));
        distance = dump_opt.draw_distance;
        distance_forced = true;
    }
    if (!vertices.create(dev, VERTEX_CAP * sizeof(ElRdVertex)) || !materials.create(dev, MATERIAL_CAP * sizeof(GpuMaterial)) ||
        !texels.create(dev, TEXEL_CAP * 4) || !palettes.create(dev, PALETTE_CAP * 4))
        return false;
    palettes.write(0, std::vector<uint32_t>(PALETTE_CAP).data(), PALETTE_CAP * 4);
    palettes.used = PALETTE_CAP * 4;
    small_buffers();
    return ensure_targets(W, H, 1);
}

// The depth buffer and its copy for decals, and with MSAA the multisampled colour: all GENERAL (a render pass,
// a copy and sampling use them without layout changes), made again when the picture's size or samples change.
// A sample count the GPU can't do falls back to fewer.
bool NativeDraw::ensure_targets(uint32_t width, uint32_t height, uint32_t samples) {
    if (depth && target_w == width && target_h == height && target_samples == samples) return true;
    // (no wait here: a command buffer is being recorded; Granite frees the old images once the GPU is done)
    depth.reset();
    depth_before.reset();
    msaa_color.reset();
    for (uint32_t n = samples; n >= 1; n /= 2) {
        ImageCreateInfo zi = ImageCreateInfo::render_target(width, height, VK_FORMAT_D32_SFLOAT);
        zi.layout = ImageLayout::General;
        zi.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        zi.samples = VkSampleCountFlagBits(n);
        depth = device->create_image(zi);
        ImageCreateInfo ci = zi;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        depth_before = device->create_image(ci);
        if (n > 1) {
            ImageCreateInfo mi = ImageCreateInfo::render_target(width, height, VK_FORMAT_R8G8B8A8_UNORM);
            mi.layout = ImageLayout::General;
            mi.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
            mi.samples = VkSampleCountFlagBits(n);
            msaa_color = device->create_image(mi);
        }
        if (depth && depth_before && (n == 1 || msaa_color)) {
            if (n != samples) std::fprintf(stderr, "[nd] MSAA %ux could not be made: %ux\n", samples, n);
            target_w = width;
            target_h = height;
            target_samples = samples; // (asked for: not tried again each picture)
            return true;
        }
        depth.reset();
        depth_before.reset();
        msaa_color.reset();
    }
    return false;
}

void NativeDraw::picture_size_1x(float aspect, uint32_t *w, uint32_t *h) {
    *h = H;
    const float a = aspect > 4.0f / 3.0f ? aspect : 4.0f / 3.0f;
    const uint32_t x = 2 * uint32_t(std::lround(a * float(H) * 0.5f));
    *w = x < W ? W : x;
}

float NativeDraw::aspect_of_1x(float aspect) {
    uint32_t w, h;
    picture_size_1x(aspect, &w, &h);
    return w == W ? 4.0f / 3.0f : float(w) / float(h);
}

void NativeDraw::shutdown() {
    if (interp && (interp->stats.plans || pstats.newest || pstats.between)) print_stats();
    print_lifetime();
    depth.reset();
    depth_before.reset();
    msaa_color.reset();
    target_w = target_h = target_samples = 0;
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

// (after a full buffer: the same content as something kept is that, so the loader's loading again what it draws
// takes no room for what the recordings kept, and keeps its ids)
uint32_t NativeDraw::texture_upload(const uint32_t *t, uint32_t count) {
    if (!kept_texture_index.empty()) {
        auto r = kept_texture_index.equal_range(fnv(t, size_t(count) * 4, count));
        for (auto k = r.first; k != r.second; ++k)
            if (k->second.count == count && !std::memcmp(texels.shadow.data() + size_t(k->second.first) * 4, t, size_t(count) * 4))
                return k->second.first;
    }
    const size_t at = texels.append(t, size_t(count) * 4);
    if (at == SIZE_MAX) return ~0u;
    textures.push_back({ uint32_t(at / 4), count });
    return uint32_t(at / 4);
}

uint32_t NativeDraw::mesh_upload(const ElRdMeshDesc *d) {
    if (!kept_mesh_index.empty()) {
        auto r = kept_mesh_index.equal_range(mesh_hash(*d));
        for (auto k = r.first; k != r.second; ++k)
            if (same_mesh(*d, k->second)) return k->second;
    }
    if (vertices.used + size_t(d->vertex_count) * sizeof(ElRdVertex) > vertices.capacity ||
        materials.used + size_t(d->material_count) * sizeof(GpuMaterial) > materials.capacity)
        return ~0u;
    Mesh m = {};
    m.first = uint32_t(vertices.append(d->vertices, size_t(d->vertex_count) * sizeof(ElRdVertex)) / sizeof(ElRdVertex));
    m.vertex_count = d->vertex_count;
    const uint32_t mat0 = uint32_t(material_cpu.size());
    m.material_first = mat0;
    m.material_count = d->material_count;
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
    const uint32_t index = uint32_t(material_cpu.size());
    material_cpu.push_back(s);
    write_material(index, s);
    materials.used = size_t(index + 1) * sizeof(GpuMaterial);
    return index;
}

// the GPU's copy of material `index`
void NativeDraw::write_material(uint32_t index, const ElRdMaterial &s) {
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
    materials.write(size_t(index) * sizeof g, &g, sizeof g);
}

uint32_t NativeDraw::material_2d(const ElRdMaterial *m) {
    if (!kept_rect_index.empty()) {
        auto r = kept_rect_index.equal_range(fnv(m, sizeof *m));
        for (auto k = r.first; k != r.second; ++k)
            if (!std::memcmp(&material_cpu[k->second], m, sizeof *m)) return k->second;
    }
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

// A buffer is full: the loader drops its caches and loads again what it draws. The ids and offsets handed out
// so far are void for the game, but the recordings (the last ticks, those waiting for their task, and the one
// being made: its items before the full buffer) still name them: those resources are kept, the rest dropped.
void NativeDraw::resources_reset() {
    if (device) device->wait_idle(); // (the GPU may still read the buffers: they are rewritten)
    keep_recorded();
}

// The game's Reset: the resources and every recording go (the game boots again; the window shows the clear
// colour until its first recording, which is shown as it is: no picture between across the reset)
void NativeDraw::reset() {
    if (device) device->wait_idle();
    palettes_of(0); // (the palette buffer the newest's: the recordings that could put older ones back go)
    hist.clear();
    pending.clear();
    building = Recording();
    in_frame = false;
    state = {};
    state_dirty = true;
    phase = 0;
    planned_a = planned_b = 0;
    planned_ok = false;
    planned_why = nullptr;
    vertices.used = materials.used = texels.used = 0;
    meshes.clear();
    groups.clear();
    material_cpu.clear();
    textures.clear();
    frame_base = (frame_last / 10000 + 1) * 10000;
    // a black picture until the game's first recording (the N64's screen at a reset; not the window's clear colour,
    // which would show for the few pictures before it). It has no camera: no picture between it and the next.
    Recording black;
    black.frame = frame_base;
    black.vi = vi_now;
    black.time = double(vi_now);
    black.serial = ++serial;
    hist.push_back(std::move(black));
    std::printf("P1_ND_RESET vi=%llu frame_base=%u\n", (unsigned long long)vi_now, frame_base);
    std::fflush(stdout);
    kept_texture_index.clear();
    kept_mesh_index.clear();
    kept_rect_index.clear();
    life.resets++;
}

void NativeDraw::keep_recorded() {
    constexpr uint32_t NONE = ~0u, USED = ~1u;
    std::vector<uint32_t> mesh_to(meshes.size(), NONE), mat_to(material_cpu.size(), NONE);
    std::vector<uint8_t> rect_material(material_cpu.size(), 0);
    auto each_recording = [&](auto &&f) {
        for (Recording &r : hist) f(r);
        for (Recording &r : pending) f(r);
        if (in_frame) f(building);
    };
    // what the recordings name: meshes (3D and 2D) and 2D rectangles' materials; a kept mesh keeps its materials
    each_recording([&](Recording &r) {
        for (const Recording::Overlay &o : r.overlays)
            if (o.material < mat_to.size()) mat_to[o.material] = USED, rect_material[o.material] = 1;
        for (const Recording::Item &it : r.items) {
            if (it.overlay >= 0) continue;
            if (it.item2d < 0) {
                if (it.draw.mesh < mesh_to.size()) mesh_to[it.draw.mesh] = USED;
                continue;
            }
            if (size_t(it.item2d) >= r.items2d.size()) continue;
            const ElRd2D &d = r.items2d[size_t(it.item2d)];
            if (d.mesh == EL_RD2D_RECT) {
                if (d.material < mat_to.size()) mat_to[d.material] = USED, rect_material[d.material] = 1;
            } else if (d.mesh < mesh_to.size()) {
                mesh_to[d.mesh] = USED;
            }
        }
    });
    for (size_t i = 0; i < meshes.size(); i++)
        if (mesh_to[i] == USED)
            for (uint32_t k = 0; k < meshes[i].material_count; k++)
                if (size_t(meshes[i].material_first) + k < mat_to.size()) mat_to[meshes[i].material_first + k] = USED;
    // the texture uploads the kept materials sample (a tile's texels lie in one upload)
    auto texture_of = [&](uint32_t texel) -> size_t {
        auto t = std::upper_bound(textures.begin(), textures.end(), texel,
                                  [](uint32_t v, const Texture &x) { return v < x.first; });
        if (t == textures.begin()) return SIZE_MAX;
        --t;
        return texel - t->first < t->count ? size_t(t - textures.begin()) : SIZE_MAX;
    };
    std::vector<uint32_t> tex_to(textures.size(), NONE);
    for (size_t m = 0; m < mat_to.size(); m++)
        if (mat_to[m] == USED)
            for (const ElRdTile &t : material_cpu[m].tile)
                if (t.format != EL_RDT_NONE) {
                    const size_t x = texture_of(t.texels);
                    if (x != SIZE_MAX) tex_to[x] = USED;
                }
    // everything kept moves down to the buffers' start in its order (a move never overwrites what is yet to
    // move): texels, then materials (their tiles' offsets moved too), then vertices, groups and meshes
    std::vector<Texture> kept_textures;
    uint32_t nt = 0;
    for (size_t x = 0; x < textures.size(); x++) {
        if (tex_to[x] != USED) continue;
        texels.move(size_t(textures[x].first) * 4, size_t(nt) * 4, size_t(textures[x].count) * 4);
        tex_to[x] = nt;
        kept_textures.push_back({ nt, textures[x].count });
        nt += textures[x].count;
    }
    uint32_t nm = 0;
    for (size_t m = 0; m < mat_to.size(); m++) {
        if (mat_to[m] != USED) continue;
        ElRdMaterial s = material_cpu[m];
        for (ElRdTile &t : s.tile)
            if (t.format != EL_RDT_NONE) {
                const size_t x = texture_of(t.texels);
                if (x != SIZE_MAX) t.texels = tex_to[x] + (t.texels - textures[x].first);
            }
        mat_to[m] = nm;
        material_cpu[nm] = s;
        write_material(nm, s);
        nm++;
    }
    std::vector<Mesh> kept_meshes;
    std::vector<Group> kept_groups;
    uint32_t nv = 0;
    for (size_t i = 0; i < meshes.size(); i++) {
        if (mesh_to[i] != USED) continue;
        const Mesh old = meshes[i];
        Mesh n = old;
        n.first = nv;
        vertices.move(size_t(old.first) * sizeof(ElRdVertex), size_t(nv) * sizeof(ElRdVertex),
                      size_t(old.vertex_count) * sizeof(ElRdVertex));
        n.group_first = uint32_t(kept_groups.size());
        for (uint32_t k = 0; k < old.group_count && size_t(old.group_first) + k < groups.size(); k++) {
            Group g = groups[old.group_first + k];
            g.first = g.first - old.first + nv;
            g.material = g.material < mat_to.size() ? mat_to[g.material] : NONE;
            kept_groups.push_back(g);
        }
        n.group_count = uint32_t(kept_groups.size()) - n.group_first;
        n.material_first = old.material_count && old.material_first < mat_to.size() ? mat_to[old.material_first] : 0;
        mesh_to[i] = uint32_t(kept_meshes.size());
        kept_meshes.push_back(n);
        nv += old.vertex_count;
    }
    // the recordings name the new numbers (one that named nothing kept names nothing: render() skips it)
    each_recording([&](Recording &r) {
        for (Recording::Overlay &o : r.overlays) o.material = o.material < mat_to.size() ? mat_to[o.material] : NONE;
        for (Recording::Item &it : r.items) {
            if (it.overlay >= 0) continue;
            if (it.item2d < 0) {
                it.draw.mesh = it.draw.mesh < mesh_to.size() ? mesh_to[it.draw.mesh] : NONE;
                continue;
            }
            if (size_t(it.item2d) >= r.items2d.size()) continue;
            ElRd2D &d = r.items2d[size_t(it.item2d)];
            if (d.mesh == EL_RD2D_RECT) d.material = d.material < mat_to.size() ? mat_to[d.material] : NONE;
            else d.mesh = d.mesh < mesh_to.size() ? mesh_to[d.mesh] : NONE;
        }
    });
    life.flushes++;
    life.kept_meshes += kept_meshes.size();
    life.kept_materials += nm;
    life.kept_texels += nt;
    std::fprintf(stderr, "[nd] buffers full: kept the recordings' %zu meshes (%u vertices), %u materials, %u texels\n",
                 kept_meshes.size(), nv, nm, nt);
    meshes = std::move(kept_meshes);
    groups = std::move(kept_groups);
    material_cpu.resize(nm);
    textures = std::move(kept_textures);
    vertices.used = size_t(nv) * sizeof(ElRdVertex);
    materials.used = size_t(nm) * sizeof(GpuMaterial);
    texels.used = size_t(nt) * 4;
    planned_a = planned_b = 0; // (the interpolator's plan named the old numbers: planned again)
    // what was kept, by content: the loader loads again what it draws, and gets these
    kept_texture_index.clear();
    kept_mesh_index.clear();
    kept_rect_index.clear();
    for (const Texture &x : textures)
        kept_texture_index.emplace(fnv(texels.shadow.data() + size_t(x.first) * 4, size_t(x.count) * 4, x.count), x);
    for (uint32_t id = 0; id < meshes.size(); id++) {
        std::vector<ElRdGroup> g;
        const ElRdMeshDesc d = mesh_desc(id, g);
        kept_mesh_index.emplace(mesh_hash(d), id);
    }
    for (size_t m = 0; m < rect_material.size(); m++)
        if (rect_material[m] && mat_to[m] < material_cpu.size())
            kept_rect_index.emplace(fnv(&material_cpu[mat_to[m]], sizeof(ElRdMaterial)), mat_to[m]);
}

// A kept mesh as the loader described it (groups relative to the mesh, into `g`)
ElRdMeshDesc NativeDraw::mesh_desc(uint32_t id, std::vector<ElRdGroup> &g) const {
    const Mesh &m = meshes[id];
    g.clear();
    for (uint32_t k = 0; k < m.group_count; k++) {
        const Group &x = groups[m.group_first + k];
        g.push_back({ x.first - m.first, x.count, x.material - m.material_first });
    }
    ElRdMeshDesc d = {};
    d.vertices = reinterpret_cast<const ElRdVertex *>(vertices.shadow.data()) + m.first;
    d.vertex_count = m.vertex_count;
    d.groups = g.data();
    d.group_count = m.group_count;
    d.materials = m.material_count ? &material_cpu[m.material_first] : nullptr;
    d.material_count = m.material_count;
    std::memcpy(d.bounds, m.bounds, sizeof d.bounds);
    return d;
}

uint64_t NativeDraw::mesh_hash(const ElRdMeshDesc &d) {
    uint64_t h = fnv(d.vertices, size_t(d.vertex_count) * sizeof(ElRdVertex), d.vertex_count);
    h = fnv(d.groups, size_t(d.group_count) * sizeof(ElRdGroup), h);
    h = fnv(d.materials, size_t(d.material_count) * sizeof(ElRdMaterial), h);
    return fnv(d.bounds, sizeof d.bounds, h);
}

bool NativeDraw::same_mesh(const ElRdMeshDesc &d, uint32_t id) const {
    std::vector<ElRdGroup> g;
    const ElRdMeshDesc k = mesh_desc(id, g);
    return d.vertex_count == k.vertex_count && d.group_count == k.group_count && d.material_count == k.material_count &&
           !std::memcmp(d.vertices, k.vertices, size_t(d.vertex_count) * sizeof(ElRdVertex)) &&
           !std::memcmp(d.groups, k.groups, size_t(d.group_count) * sizeof(ElRdGroup)) &&
           (!d.material_count || !std::memcmp(d.materials, k.materials, size_t(d.material_count) * sizeof(ElRdMaterial))) &&
           !std::memcmp(d.bounds, k.bounds, sizeof d.bounds);
}

// ELTALE_ND_SMALL_BUFFERS=n (a test): the vertex, material and texel buffers hold n KiB each, so they fill in play
// and resources_reset runs often
void NativeDraw::small_buffers() {
    const char *e = std::getenv("ELTALE_ND_SMALL_BUFFERS");
    if (!e || std::atol(e) <= 0) return;
    const size_t bytes = size_t(std::atol(e)) * 1024;
    vertices.capacity = std::min(bytes, vertices.capacity);
    materials.capacity = std::min(bytes, materials.capacity);
    texels.capacity = std::min(bytes, texels.capacity);
    small = true;
    std::fprintf(stderr, "[nd] small buffers: %zu vertices, %zu materials, %zu texels\n", vertices.capacity / sizeof(ElRdVertex),
                 materials.capacity / sizeof(GpuMaterial), texels.capacity / 4);
}

// P1_ND_LIFETIME at exit: the resets, the full buffers and what they kept, the stale items render() skipped
void NativeDraw::print_lifetime() const {
    if (!life.resets && !life.flushes && !life.stale && !small) return;
    std::printf("P1_ND_LIFETIME resets=%llu flushes=%llu kept_meshes=%llu kept_materials=%llu kept_texels=%llu stale=%llu\n",
                (unsigned long long)life.resets, (unsigned long long)life.flushes, (unsigned long long)life.kept_meshes,
                (unsigned long long)life.kept_materials, (unsigned long long)life.kept_texels, (unsigned long long)life.stale);
    std::fflush(stdout);
}

// Does the item name resources that are there? (its state, its 2D item, its mesh's groups, their materials and
// vertices, a rectangle's material)
bool NativeDraw::item_ok(const Recording &rec, const Recording::Item &it) const {
    if (it.state >= rec.states.size()) return false;
    if (it.overlay >= 0) {
        if (size_t(it.overlay) >= rec.overlays.size()) return false;
        const Recording::Overlay &o = rec.overlays[size_t(it.overlay)];
        return o.material < material_cpu.size() && size_t(o.first) + o.count <= rec.overlay_vertices.size();
    }
    uint32_t id = it.draw.mesh;
    if (it.item2d >= 0) {
        if (size_t(it.item2d) >= rec.items2d.size()) return false;
        const ElRd2D &d = rec.items2d[size_t(it.item2d)];
        if (d.mesh == EL_RD2D_RECT) return d.material < material_cpu.size();
        id = d.mesh;
    }
    if (id >= meshes.size()) return false;
    const Mesh &m = meshes[id];
    if (size_t(m.group_first) + m.group_count > groups.size()) return false;
    const size_t vertex_count = vertices.used / sizeof(ElRdVertex);
    for (uint32_t k = 0; k < m.group_count; k++) {
        const Group &g = groups[m.group_first + k];
        if (g.material >= material_cpu.size() || size_t(g.first) + g.count > vertex_count) return false;
    }
    return true;
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
    building.overlays.clear();
    building.overlay_vertices.clear();
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

// v10: world-space triangles of the port's own, in the recording's order (drawn with the picture's camera)
void NativeDraw::overlay(const ElRdVertex *v, uint32_t count, uint32_t material) {
    if (!in_frame || !count || material >= material_cpu.size()) return;
    count -= count % 3;
    if (state_dirty || building.states.empty()) {
        building.states.push_back(state);
        state_dirty = false;
    }
    Recording::Overlay o = { uint32_t(building.overlay_vertices.size()), count, material };
    building.overlay_vertices.insert(building.overlay_vertices.end(), v, v + count);
    Recording::Item it = {};
    it.state = uint32_t(building.states.size() - 1);
    it.overlay = int32_t(building.overlays.size());
    // (an id of its own and an identity matrix: pictures between ticks leave it as it is)
    it.draw.id = uint64_t(0xFE) << EL_RDD_KIND_SHIFT | uint64_t(building.overlays.size());
    it.draw.matrix[0] = it.draw.matrix[5] = it.draw.matrix[10] = it.draw.matrix[15] = 1.0f;
    building.overlays.push_back(o);
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
    // (after a game Reset the game counts from 1 again: the recordings go on from the next 10000, so the dumps
    // and P1_ lines name the ticks before and after a reset apart)
    r.frame = frame + frame_base;
    frame = r.frame;
    frame_last = frame;
    r.vi = vi_now;
    r.time = double(vi_now) + double(phase);
    phase = 0;
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
    const Recording *r = pick(v, between);
    return r;
}

// the recordings the last picture was made from (the area map's arrow follows them: docs/AUTOMAP.md)
void NativeDraw::note_shown(const Recording *a, const Recording *b, double t) {
    // (the game's own numbers: recordings after a reset count on from frame_base)
    shown.a = a ? a->frame - frame_base : 0;
    shown.b = b ? b->frame - frame_base : shown.a;
    shown.t = b ? float(t < 0 ? 0 : t > 1 ? 1 : t) : 0.0f;
    shown.valid = a != nullptr;
}

const Recording *NativeDraw::pick(double v, bool between) {
    if (hist.empty()) { note_shown(nullptr, nullptr, 0); return nullptr; }
    if (!between) { note_shown(&hist.back(), nullptr, 0); return &hist.back(); }
    // the newest recording due by v
    int k = -1;
    for (int i = int(hist.size()) - 1; i >= 0; i--)
        if (hist[size_t(i)].time + BETWEEN_DELAY <= v) { k = i; break; }
    if (k < 0) { // none due yet: the oldest kept (a start, after a reset)
        pstats.fallback++;
        pstats.early++;
        note_shown(&hist.front(), nullptr, 0);
        return &hist.front();
    }
    if (size_t(k) + 1 == hist.size()) { // nothing newer yet: the newest as it is
        pstats.newest++;
        note_shown(&hist.back(), nullptr, 0);
        return &hist.back();
    }
    const Recording &a = hist[size_t(k)], &b = hist[size_t(k) + 1];
    const double span = b.time - a.time, t = (v - (a.time + BETWEEN_DELAY)) / span;
    note_shown(&a, nullptr, 0);
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
    note_shown(&a, &b, t);
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
    // R7: the picture's size (picture_size_1x for the aspect, 320x240 at 4:3, times the resolution scale) and MSAA
    const uint32_t pw = picture.get_width(), ph = picture.get_height();
    const float scale = float(ph) / float(H); // (picture pixels per N64 pixel: the height is always 240 of them)
    if (!ensure_targets(pw, ph, opt.samples ? opt.samples : 1)) return;
    const uint32_t samples = msaa_color ? uint32_t(msaa_color->get_create_info().samples) : 1u;
    // 2D edges on the picture's pixel edges (a bigger picture or MSAA; at 1x without MSAA nothing moves): below
    const bool crisp = ph != H || samples > 1;
    vertices.upload(cmd);
    materials.upload(cmd);
    texels.upload(cmd);
    palettes.upload(cmd);
    cmd.barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    // R7a (widescreen): the picture is 240 units high and vw wide; 2D comes in a 320-wide screen's units and
    // is moved by its anchor (render.h EL_RD2D_ANCHOR_*): `side` is what a wider picture adds at each side.
    // (picture_size_1x makes the 1x width even, so side is a whole number of N64 pixels: the anchors keep 2D
    // on the N64 pixel grid at any scale, which OPT_RECT's crisp sampling relies on)
    const float vw = float(H) * float(pw) / float(ph), side = (vw - float(W)) * 0.5f;
    const bool wide = side > 0.25f;
    auto anchor_dx = [&](uint32_t flags) -> float {
        switch (flags & EL_RD2D_ANCHOR_MASK) {
        case EL_RD2D_ANCHOR_LEFT: return 0.0f;
        case EL_RD2D_ANCHOR_RIGHT: return 2.0f * side;
        default: return side; // CENTRE, WORLD (STRETCH: its left edge 0, its right 2 x side)
        }
    };

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
            if (!item_ok(*rec, it)) {
                // a stale item (ids from before a reset or a full buffer): never drawn. A debug build stops here
                assert(!"native draw: an item names a mesh or material that is not there");
                life.stale++;
                options[i] = OPT_SKIP;
                continue;
            }
            const Recording::State &s = rec->states[it.state];
            GpuDraw &g = gd[i];
            std::memset(&g, 0, sizeof g);
            if (it.overlay >= 0) {
                // v10: world-space triangles (their vertices after the rectangles' in this picture's buffer), the
                // camera alone, the depth as a 3D draw's
                if (!camera_ok) {
                    options[i] = OPT_SKIP;
                    continue;
                }
                const Recording::Overlay &o = rec->overlays[size_t(it.overlay)];
                rect_first.resize(i + 1, 0);
                rect_first[i] = uint32_t(rect_vertices.size());
                rect_vertices.insert(rect_vertices.end(), rec->overlay_vertices.begin() + o.first,
                                     rec->overlay_vertices.begin() + o.first + o.count);
                std::memcpy(g.mvp, cam.projection, sizeof g.mvp);
                g.colors[0] = pack(s.env);
                g.colors[1] = pack(s.prim);
                g.colors[2] = pack(s.fog);
                g.colors[3] = pack(s.blend);
                g.fog[0] = s.fog_mul;
                g.fog[1] = s.fog_off;
                g.fog[2] = cam.near_plane > 0 ? cam.near_plane : 1.0f;
                g.fog[3] = 511.0f * 32.0f * std::fabs(cam.perspective[14]) / g.fog[2];
                g.tile_offset[2] = -cam.perspective[10];
                g.tile_offset[3] = 1.0f;
                // v11 (EL_RDV_RIM): this picture's eye, from its own view (between ticks too: the rim turns with the
                // camera), and the rim's constants
                overlay_eye(cam.view, g.light_dir[0]);
                g.light_col[0][0] = EL_RD_RIM_FLOOR;
                g.light_col[0][1] = EL_RD_RIM_POWER;
                g.light_col[0][2] = EL_RD_RIM_SOFT;
                options[i] = 0;
                continue;
            }
            if (it.item2d >= 0) {
                const ElRd2D &d = rec->items2d[size_t(it.item2d)];
                if (d.mesh == EL_RD2D_RECT) {
                    // pixels to clip space (x right, y down; the vertex stage flips y back)
                    const float screen[16] = { 2.0f / vw, 0, 0, 0, 0, -1.0f / 120, 0, 0, 0, 0, 0, 0, -1, 1, 0, 1 };
                    std::memcpy(g.mvp, screen, sizeof screen);
                    const bool stretch = (d.flags & EL_RD2D_ANCHOR_MASK) == EL_RD2D_ANCHOR_STRETCH;
                    const float dx = stretch ? 0.0f : anchor_dx(d.flags);
                    // the RDP's texel at pixel (x, y): s0 + (x - x0) * dsdx; a little over, so a texel
                    // boundary the interpolation lands just under still floors to the right texel
                    const float eps = 1.0f / 256, x0 = d.rect[0], y0 = d.rect[1], x1 = d.rect[2], y1 = d.rect[3];
                    // (in a bigger picture, or with MSAA, the edges go to the N64 pixels the 1x picture covers: a
                    // pixel is in when its corner is, so a fractional edge rounds up, and no part-row of a texel shows)
                    float ex0 = x0, ey0 = y0, ex1 = x1, ey1 = y1;
                    if (crisp) ex0 = std::ceil(x0), ey0 = std::ceil(y0), ex1 = std::ceil(x1), ey1 = std::ceil(y1);
                    // ... and lie on the picture's pixel edges, not on its pixel centres: the viewport's half-pixel
                    // move puts an edge at N64 x on picture pixel x * scale + 0.5, which a single sample decides by
                    // the rounding of the edge, and MSAA's samples (either side of the centre) took as half covered:
                    // every rectangle got a half-blended line of the texel past its last row and column (the HUD
                    // digits' lines, the spell panel's line at its right). Back by half a picture pixel the edges are
                    // whole picture pixels; s and t stay the same function of the place, so each pixel still samples
                    // its corner (OPT_RECT then its N64 pixel's)
                    const float back = crisp ? 0.5f / scale : 0.0f;
                    auto vert = [&](float x, float y) {
                        ElRdVertex v = {};
                        v.x = x - back + (stretch ? (x > ex0 ? 2.0f * side : 0.0f) : dx); // (STRETCH: its right edge)
                        v.y = y - back;
                        v.s = d.st[0] + (x - back - x0) * d.st[2] + eps;
                        v.t = d.st[1] + (y - back - y0) * d.st[3] + eps;
                        rect_vertices.push_back(v);
                    };
                    rect_first.resize(i + 1, 0);
                    rect_first[i] = uint32_t(rect_vertices.size());
                    vert(ex0, ey0); vert(ex1, ey0); vert(ex1, ey1);
                    vert(ex0, ey0); vert(ex1, ey1); vert(ex0, ey1);
                } else {
                    std::memcpy(g.mvp, d.matrix, sizeof g.mvp);
                    if (wide) {
                        // the 4:3 clip space's x into the wide picture's, moved by the anchor:
                        // x' = x * 320 / vw + w * ((320 + 2 dx) / vw - 1)
                        const float k = float(W) / vw, c = (float(W) + 2.0f * anchor_dx(d.flags)) / vw - 1.0f;
                        for (int r = 0; r < 4; r++) g.mvp[r * 4] = d.matrix[r * 4] * k + d.matrix[r * 4 + 3] * c;
                    }
                }
                g.colors[0] = pack(d.env);
                g.colors[1] = pack(d.prim);
                g.colors[2] = pack(d.fog);
                g.colors[3] = pack(d.blend);
                g.fog[2] = 0.0f; // the depth written (no depth test for 2D)
                g.fog[3] = 1.0f;
                options[i] = (opt.three_point ? OPT_THREE_POINT : 0) | (d.mesh == EL_RD2D_RECT ? OPT_RECT : 0);
                continue;
            }
            if (!camera_ok && !(it.draw.flags & EL_RDD_SCREEN)) {
                options[i] = OPT_SKIP;
                continue;
            }
            if (it.draw.flags & EL_RDD_SCREEN) {
                std::memcpy(g.mvp, it.draw.matrix, sizeof g.mvp);
                // a screen draw's edges (the letterbox, the fade) are N64 pixel edges too: back by half a picture
                // pixel, as a rectangle's, or MSAA took the first picture row past each edge as half covered
                if (crisp) {
                    const float tx = -1.0f / float(pw), ty = 1.0f / float(ph); // (clip y is flipped by the vertex stage)
                    for (int r = 0; r < 4; r++) {
                        g.mvp[r * 4] += tx * g.mvp[r * 4 + 3];
                        g.mvp[r * 4 + 1] += ty * g.mvp[r * 4 + 3];
                    }
                }
            }
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
            // R7 draw distance k: the fog and the far plane as if everything were k times nearer. The
            // perspective's z/w is A - C/w (A = -m[2][2]): z/w at w/k is A + k (z/w - A), so the vertex stage
            // takes z' = A w + k (z - A w) for the fog and the far plane (tile_offset[2] = A, [3] = k)
            const float k_far = (it.draw.flags & EL_RDD_SCREEN) ? 1.0f : std::max(opt.draw_distance, 1.0f);
            const float a_far = -cam.perspective[10];
            g.tile_offset[2] = a_far;
            g.tile_offset[3] = k_far;
            // does the mesh's box reach past the far plane? (then the pixel stage cuts there)
            uint32_t o = (opt.three_point ? OPT_THREE_POINT : 0) | (opt.n64_fog ? OPT_N64_FOG : 0);
            const Mesh &mesh = meshes[it.draw.mesh];
            for (int k = 0; k < 8 && !(o & OPT_FAR_CLIP); k++) {
                const float p[3] = { mesh.bounds[(k & 1) ? 3 : 0], mesh.bounds[(k & 2) ? 4 : 1], mesh.bounds[(k & 4) ? 5 : 2] };
                float z = p[0] * g.mvp[2] + p[1] * g.mvp[6] + p[2] * g.mvp[10] + g.mvp[14];
                const float w = p[0] * g.mvp[3] + p[1] * g.mvp[7] + p[2] * g.mvp[11] + g.mvp[15];
                if (k_far != 1.0f) z = a_far * w + k_far * (z - a_far * w);
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
        if (!gd.empty() && rec->items[0].item2d < 0 && rec->items[0].overlay < 0 && !(options[0] & OPT_SKIP)) {
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
    // MSAA: drawn into the multisampled colour, resolved into the picture at the end of each pass (kept: a
    // decal's depth copy ends the pass, and a new one goes on drawing into it)
    RenderPassInfo::Subpass subpass = {};
    Image &target = msaa_color ? *msaa_color : picture;
    if (msaa_color) {
        rp.num_color_attachments = 2;
        rp.color_attachments[0] = &msaa_color->get_view();
        rp.color_attachments[1] = &picture.get_view();
        rp.store_attachments = 3;
        subpass.num_color_attachments = 1;
        subpass.color_attachments[0] = 0;
        subpass.num_resolve_attachments = 1;
        subpass.resolve_attachments[0] = 1;
        subpass.depth_stencil_mode = RenderPassInfo::DepthStencil::ReadWrite;
        rp.subpasses = &subpass;
        rp.num_subpasses = 1;
    }
    Program *prog = msaa_color ? program_ms : program;
    cmd.begin_render_pass(rp);
    // the border: 8 N64 pixels, at the picture's scale
    const int32_t b8 = int32_t(std::lround(8 * scale));
    const VkRect2D inner = { { b8, b8 }, { pw - 2 * uint32_t(b8), ph - 2 * uint32_t(b8) } };
    const VkRect2D whole = opt.border ? inner : VkRect2D{ { 0, 0 }, { pw, ph } };
    // R7: a CENTRE 2D item is cut to the picture's 4:3 middle (pillarbox: what a 4:3 screen hides stays hidden)
    VkRect2D middle = whole;
    if (wide) {
        const int32_t x0 = int32_t(std::lround(side * scale)), x1 = int32_t(std::lround((side + float(W)) * scale));
        middle.offset.x = std::max(whole.offset.x, x0);
        middle.extent.width = uint32_t(std::min(int32_t(whole.offset.x + whole.extent.width), x1) - middle.offset.x);
    }
    const VkRect2D *scissor_now = nullptr;
    auto scissor = [&](const VkRect2D &r) {
        if (scissor_now == &r) return;
        cmd.set_scissor(r);
        scissor_now = &r;
    };
    if (opt.border) {
        VkClearValue v = {};
        v.color = clear_colour;
        cmd.clear_quad(0, { inner, 0, 1 }, v, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    // the state a render pass starts with (again after a decal's depth copy)
    auto setup = [&]() {
        cmd.set_opaque_state();
        cmd.set_program(prog);
        cmd.set_storage_buffer(0, 0, *vertices.buffer);
        cmd.set_storage_buffer(0, 1, *draw_buffer);
        cmd.set_storage_buffer(0, 2, *materials.buffer);
        cmd.set_storage_buffer(0, 3, *texels.buffer);
        cmd.set_storage_buffer(0, 4, *palettes.buffer);
        cmd.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        cmd.set_front_face(VK_FRONT_FACE_COUNTER_CLOCKWISE);
        // the RDP samples a pixel at its top-left corner, the GPU at its centre: the picture moves half a pixel
        // (of the picture's: at a scale each pixel is sampled at its own corner, so 2D stays on whole pixels)
        cmd.set_viewport({ 0.5f, 0.5f, float(pw), float(ph), 0, 1 });
        scissor_now = nullptr;
        scissor(whole);
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
        cmd.image_barrier(target, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        cmd.image_barrier(*depth, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        cmd.image_barrier(*depth_before, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        if (msaa_color)
            cmd.image_barrier(picture, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        RenderPassInfo again = rp;
        again.clear_attachments = 0;
        again.load_attachments = 1;
        again.op_flags = RENDER_PASS_OP_LOAD_DEPTH_STENCIL_BIT | RENDER_PASS_OP_STORE_DEPTH_STENCIL_BIT;
        cmd.begin_render_pass(again);
        setup();
    };
    const uint32_t dither = opt.n64_dither ? OPT_DITHER : 0u;
    if (!gd.empty()) {
        setup();
        bool copied = false, written = false;
        bool rect_bound = false;
        for (size_t i = 0; i < rec->items.size(); i++) {
            if (options[i] & OPT_SKIP) continue;
            const Recording::Item &item = rec->items[i];
            const ElRd2D *d2 = item.item2d >= 0 ? &rec->items2d[size_t(item.item2d)] : nullptr;
            if (wide) scissor(d2 && (d2->flags & EL_RD2D_ANCHOR_MASK) == EL_RD2D_ANCHOR_CENTRE ? middle : whole);
            if (item.overlay >= 0) {
                const Recording::Overlay &o = rec->overlays[size_t(item.overlay)];
                if (!rect_bound) cmd.set_storage_buffer(0, 0, *rect_buffer);
                rect_bound = true;
                apply_state(cmd, material_cpu[o.material]);
                Push push = { uint32_t(i), o.material, options[i] | dither, view };
                cmd.push_constants(&push, 0, sizeof push);
                cmd.draw(o.count, 1, rect_first[i], 0);
                continue;
            }
            if (d2 && d2->mesh == EL_RD2D_RECT) {
                if (!rect_bound) cmd.set_storage_buffer(0, 0, *rect_buffer);
                rect_bound = true;
                const ElRdMaterial &mat = material_cpu[d2->material];
                apply_state(cmd, mat);
                Push push = { uint32_t(i), d2->material, options[i] | dither, view };
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
                apply_state(cmd, mat, samples > 1 && !d2 && is_cutout(mat));
                Push push = { uint32_t(i), grp.material, options[i] | dither | (decal ? OPT_DECAL : 0u), view };
                cmd.push_constants(&push, 0, sizeof push);
                cmd.draw(grp.count, 1, grp.first, 0);
            }
        }
    }
    cmd.end_render_pass();
}

void NativeDraw::dump_frame(const Recording &rec, const std::string &name) {
    if (!device) return;
    // the N64 look (the border, three-point filtering, the N64's fog) at 1x, unless ELTALE_DUMP_* say otherwise
    DrawOptions opt = dump_opt;
    dump_picture_at(rec, name, dump_scale, opt);
    if (dump_pair) {
        opt.samples = 1;
        dump_picture_at(rec, name + ".1x", 1, opt);
    }
    if (name.compare(0, 6, "frame_") == 0 && name.find('_', 6) == std::string::npos)
        std::printf("P1_NATIVE_FRAME frame=%u items=%zu\n", rec.frame, rec.items.size());
    std::fflush(stdout);
}

void NativeDraw::dump_picture_at(const Recording &rec, const std::string &name, uint32_t scale, const DrawOptions &opt_in) {
    uint32_t w1, h1; // R7a: the picture's shape (ELTALE_WIDESCREEN) at 1x, times the scale
    picture_size_1x(aspect_value, &w1, &h1);
    const uint32_t dw = w1 * scale, dh = h1 * scale;
    if (dump_picture && (dump_picture->get_width() != dw || dump_picture->get_height() != dh)) dump_picture.reset();
    if (!dump_picture) {
        ImageCreateInfo info = ImageCreateInfo::render_target(dw, dh, VK_FORMAT_R8G8B8A8_UNORM);
        info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        dump_picture = device->create_image(info);
    }
    // (widescreen: no border; the 4:3 dumps keep the N64 look with it)
    DrawOptions opt = opt_in;
    if (w1 != W) opt.border = false;
    CommandBufferHandle cmd = device->request_command_buffer();
    cmd->full_barrier();
    render(*cmd, *dump_picture, &rec, opt);
    BufferCreateInfo bi = {};
    bi.size = dw * dh * 4;
    bi.domain = BufferDomain::CachedHost;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    BufferHandle buffer = device->create_buffer(bi);
    cmd->barrier(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    cmd->copy_image_to_buffer(*buffer, *dump_picture, 0, { 0, 0, 0 }, { dw, dh, 1 }, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });
    cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                 VK_ACCESS_2_HOST_READ_BIT);
    Fence fence;
    device->submit(cmd, &fence);
    fence->wait();
    const uint32_t *px = static_cast<const uint32_t *>(device->map_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT));
    std::vector<uint32_t> rgba(px, px + dw * dh);
    device->unmap_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT);
    for (uint32_t &p : rgba) p |= 0xFF000000u;
    const std::string path = dump_dir + "/" + name + ".png";
    if (!render_png_write(path.c_str(), rgba.data(), dw, dh)) std::fprintf(stderr, "[render] cannot write %s\n", path.c_str());
    device->next_frame_context();
}

} // namespace render
