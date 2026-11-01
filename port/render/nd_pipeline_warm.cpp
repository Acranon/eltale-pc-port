// Pipelines made ahead (nd_pipeline_warm.h; docs/renderer/NATIVE_DRAW.md "Modern options"). From King's Field IV
// (holyland, MIT): port/gs/native/gsn_pipeline_warm.inc, its save of the pipeline cache and its list format.
//
// A Vulkan pipeline is made the first time a draw needs it. The native renderer has one program (an uber shader:
// the material is data), so its pipelines are only the GPU state combinations (depth, cull, blend, sample shading)
// times the render passes (the MSAA sample count); but its fragment shader is big, and a pipeline made from
// nothing (a fresh data folder, a new build, a new driver, the driver's own cache full or evicting) costs the
// driver tens of milliseconds to seconds, while the picture waits: the hitch of a first visit to a map, a spell,
// a menu. So every pipeline Granite makes is written down (its shaders by SPIR-V hash and resource layout, its
// compatible render pass by the attachments' formats and samples, its state words) in a list shipped in the
// program (nd_pipeline_list.h, from tools/pipeline_list.py over runs' lists) and, for pipelines that list lacks,
// the data folder's native_pipeline_list.txt. At the start the lines tagged with this sample count are made again
// on threads of their own (first those the pipeline cache has, at once; then the rest), each checked against the
// hash Granite will look it up by (a line of another build that no longer makes the same pipeline is skipped:
// stale). A draw that needs one not made yet still makes it then. The pipeline cache (native_pipelines.bin) and the
// list are written two seconds after pipelines were made, and at the end.
//
// Each line of a list: "S hash layout" (a shader's resource layout), "R hash pass" (a compatible render pass: its
// attachments), "G tag hash vs fs pass subpass subgroup state potential attribs strides rates" (a graphics
// pipeline), the blobs in hex with their trailing zero bytes left out. The lists hold no game data.
//
// ELTALE_WARM=0 makes nothing ahead (the lists are still kept); ELTALE_WARM_THREADS=N. P1_PIPELINES at the end:
// the pipelines made ahead, and those a draw waited for (made in play) with the time they took: the hitches.

// Granite first (it brings Vulkan's headers through volk)
#include "device.hpp"
#include "command_buffer.hpp"
#include "thread_id.hpp"

#include "nd_pipeline_warm.h"
#include "nd_pipeline_list.h" // (the shipped list: nd_pipeline_list[])
#include "nd_shaders.h"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Vulkan {
// cmake/granite_pipeline_warm.inc
extern void (*holyland_on_compile)(Device *device, const DeferredPipelineCompile &compile, bool compute, int64_t ns,
                                   bool sync, bool ok);
extern void (*holyland_on_render_pass)(Util::Hash hash, const RenderPassInfo &info);
} // namespace Vulkan

namespace render {
namespace {

using namespace Vulkan;

struct Spirv {
    const uint32_t *code;
    size_t size;
};
const Spirv spirv_all[] = {
    { nd_spv_nd_vert, sizeof(nd_spv_nd_vert) },
    { nd_spv_nd_frag, sizeof(nd_spv_nd_frag) },
    { nd_spv_nd_ms_frag, sizeof(nd_spv_nd_ms_frag) },
    { nd_spv_present_vert, sizeof(nd_spv_present_vert) },
    { nd_spv_present_area_frag, sizeof(nd_spv_present_area_frag) },
};

// The presenter's own pipelines (render_native.cpp's area filter for Smooth pixel edges: present_area.frag) are
// tagged "p", not by the render pass's samples (its pass is always one sample, whatever the MSAA): they are made
// ahead at the start whatever the MSAA count.
const char *const PRESENT_TAG = "p";
bool pw_is_present(uint64_t fs_hash) {
    static const uint64_t area = Shader::hash(nd_spv_present_area_frag, sizeof(nd_spv_present_area_frag));
    return fs_hash == area;
}

// a compatible render pass: what Device::request_render_pass hashes, and the samples it makes the pass with
struct PwPass {
    uint32_t colours;
    uint32_t colour_format[VULKAN_NUM_ATTACHMENTS], colour_samples[VULKAN_NUM_ATTACHMENTS];
    uint32_t lazy; // (attachments in transient images: a bit each, the depth's after the colours')
    uint32_t depth_format, depth_samples;
    uint32_t base_layer, layers, subpasses;
    RenderPassInfo::Subpass subpass[4];
};
struct PwEntry {
    std::string tag;
    uint64_t hash = 0;
    uint64_t shader[2] = {};
    uint64_t pass = 0;
    uint32_t subpass = 0, subgroup = 0;
    PipelineState state = {};
    PotentialState potential = {};
    VertexAttribState attribs[VULKAN_NUM_VERTEX_ATTRIBS] = {};
    VkDeviceSize strides[VULKAN_NUM_VERTEX_BUFFERS] = {};
    VkVertexInputRate rates[VULKAN_NUM_VERTEX_BUFFERS] = {};
    bool shipped = false;
};
struct PipelineWarm {
    Device *device = nullptr;
    bool warm = true;
    std::mutex lock;
    std::unordered_map<uint64_t, std::vector<uint8_t>> shaders; // a shader's ResourceLayout, serialized
    std::unordered_map<uint64_t, PwPass> passes;
    std::vector<PwEntry> entries;
    std::set<std::pair<std::string, uint64_t>> known; // (tag, hash)
    std::set<uint32_t> started;                       // the sample counts made ahead (or being made)
    std::string list_path, cache_path;
    size_t saved = 0, unshipped = 0;
    size_t cache_saved_size = 0;
    std::atomic<int> total{0}, done{0}, ready{0}, stale{0}, running{0};
    std::atomic<bool> stop{false};
    double seconds_build = 0, waited_ms = 0;
    std::atomic<uint64_t> play_made{0}, play_ns{0}, play_max_ns{0}, play_new{0}, ahead_ns{0};
    std::atomic<uint64_t> made{0};
    uint64_t made_saved = 0;
    std::chrono::steady_clock::time_point made_at;
    std::vector<std::thread> threads;
} pw;
thread_local bool pw_starting; // (pw_start's thread, holding pw.lock: Granite's hooks leave it be)

std::string pw_hex(const void *data, size_t size) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    while (size && !p[size - 1]) size--;
    std::string s(size * 2, '0');
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; i++) {
        s[i * 2] = digits[p[i] >> 4];
        s[i * 2 + 1] = digits[p[i] & 15];
    }
    return s.empty() ? "-" : s;
}
bool pw_unhex(const std::string &s, void *data, size_t size) {
    uint8_t *p = static_cast<uint8_t *>(data);
    std::memset(p, 0, size);
    if (s == "-") return true;
    if (s.size() % 2 || s.size() / 2 > size) return false;
    for (size_t i = 0; i < s.size() / 2; i++) {
        unsigned v;
        if (std::sscanf(s.c_str() + i * 2, "%2x", &v) != 1) return false;
        p[i] = uint8_t(v);
    }
    return true;
}

// the setting a pipeline was made at that decides which pipelines there are: the MSAA sample count
std::string pw_tag(uint32_t samples) { return "m" + std::to_string(samples); }

std::string pw_line(const PwEntry &e) {
    char head[160];
    std::snprintf(head, sizeof head, "G %s %016llx %016llx %016llx %016llx %u %u ", e.tag.c_str(), (unsigned long long)e.hash,
                  (unsigned long long)e.shader[0], (unsigned long long)e.shader[1], (unsigned long long)e.pass, e.subpass,
                  e.subgroup);
    return head + pw_hex(&e.state, sizeof e.state) + " " + pw_hex(&e.potential, sizeof e.potential) + " " +
           pw_hex(e.attribs, sizeof e.attribs) + " " + pw_hex(e.strides, sizeof e.strides) + " " + pw_hex(e.rates, sizeof e.rates);
}

// one line of a list (pw.lock held)
void pw_read_line(const std::string &line, bool shipped) {
    std::vector<std::string> f;
    size_t at = 0;
    while (at < line.size()) {
        size_t end = line.find(' ', at);
        if (end == std::string::npos) end = line.size();
        if (end > at) f.push_back(line.substr(at, end - at));
        at = end + 1;
    }
    if (f.empty()) return;
    auto u64 = [](const std::string &s) { return (uint64_t)std::strtoull(s.c_str(), nullptr, 16); };
    if (f[0] == "S" && f.size() == 3) {
        std::vector<uint8_t> layout(ResourceLayout::serialization_size());
        if (pw_unhex(f[2], layout.data(), layout.size())) pw.shaders.emplace(u64(f[1]), layout);
    } else if (f[0] == "R" && f.size() == 3) {
        PwPass p;
        if (pw_unhex(f[2], &p, sizeof p)) pw.passes.emplace(u64(f[1]), p);
    } else if (f[0] == "G" && f.size() == 13) {
        PwEntry e;
        e.tag = f[1];
        e.hash = u64(f[2]);
        e.shader[0] = u64(f[3]);
        e.shader[1] = u64(f[4]);
        e.pass = u64(f[5]);
        e.subpass = uint32_t(std::strtoul(f[6].c_str(), nullptr, 10));
        e.subgroup = uint32_t(std::strtoul(f[7].c_str(), nullptr, 10));
        const bool ok = pw_unhex(f[8], &e.state, sizeof e.state) && pw_unhex(f[9], &e.potential, sizeof e.potential) &&
                        pw_unhex(f[10], e.attribs, sizeof e.attribs) && pw_unhex(f[11], e.strides, sizeof e.strides) &&
                        pw_unhex(f[12], e.rates, sizeof e.rates);
        e.shipped = shipped;
        if (ok && pw.known.emplace(e.tag, e.hash).second) {
            pw.entries.push_back(e);
            pw.unshipped += !shipped;
        }
    }
}

bool write_file(const std::string &path, const void *data, size_t size) {
    const std::string temp = path + ".new"; // (beside it, then in its place: never half written)
    FILE *f = std::fopen(temp.c_str(), "wb");
    if (!f) return false;
    bool written = std::fwrite(data, 1, size, f) == size;
    written = std::fclose(f) == 0 && written;
    if (!written) return false;
    std::remove(path.c_str());
    return std::rename(temp.c_str(), path.c_str()) == 0;
}

// The data folder's list, written again when it has more (the shaders and passes its lines name, then its lines)
void pw_save_list() {
    std::string text;
    {
        std::lock_guard<std::mutex> hold(pw.lock);
        if (pw.list_path.empty() || pw.unshipped == pw.saved) return;
        std::set<uint64_t> shaders, passes;
        std::string lines;
        for (const PwEntry &e : pw.entries) {
            if (e.shipped) continue;
            shaders.insert(e.shader[0]);
            shaders.insert(e.shader[1]);
            passes.insert(e.pass);
            lines += pw_line(e) + "\n";
        }
        text = "# eltale: pipelines made in play that the shipped list does not have (nd_pipeline_warm.cpp)\n";
        for (uint64_t h : shaders) {
            auto it = pw.shaders.find(h);
            if (it == pw.shaders.end()) continue;
            char head[32];
            std::snprintf(head, sizeof head, "S %016llx ", (unsigned long long)h);
            text += head + pw_hex(it->second.data(), it->second.size()) + "\n";
        }
        for (uint64_t h : passes) {
            auto it = pw.passes.find(h);
            if (it == pw.passes.end()) continue;
            char head[32];
            std::snprintf(head, sizeof head, "R %016llx ", (unsigned long long)h);
            text += head + pw_hex(&it->second, sizeof it->second) + "\n";
        }
        text += lines;
        pw.saved = pw.unshipped;
    }
    write_file(pw.list_path, text.data(), text.size());
}

void pw_save_cache() {
    if (!pw.device || pw.cache_path.empty()) return;
    const size_t size = pw.device->get_pipeline_cache_size();
    if (size == 0 || size == pw.cache_saved_size) return;
    std::vector<uint8_t> data(size);
    if (!pw.device->get_pipeline_cache_data(data.data(), size)) return;
    if (write_file(pw.cache_path, data.data(), size)) pw.cache_saved_size = size;
}

// ---- Granite's hooks (cmake/granite_pipeline_warm.inc) -----------------------------------------------------------
void pw_on_render_pass(Util::Hash hash, const RenderPassInfo &info) {
    if (pw_starting) return; // (made again from the list: it has it)
    if (info.num_color_attachments > VULKAN_NUM_ATTACHMENTS || info.num_subpasses > 4) return;
    PwPass p = {};
    p.colours = info.num_color_attachments;
    for (unsigned i = 0; i < info.num_color_attachments; i++) {
        const Image &image = info.color_attachments[i]->get_image();
        if (image.get_swapchain_layout() != VK_IMAGE_LAYOUT_UNDEFINED) return; // (a window's: not made again)
        p.colour_format[i] = info.color_attachments[i]->get_format();
        p.colour_samples[i] = image.get_create_info().samples;
        p.lazy |= uint32_t(image.get_create_info().domain == ImageDomain::Transient) << i;
    }
    if (info.depth_stencil) {
        const Image &image = info.depth_stencil->get_image();
        p.depth_format = info.depth_stencil->get_format();
        p.depth_samples = image.get_create_info().samples;
        p.lazy |= uint32_t(image.get_create_info().domain == ImageDomain::Transient) << info.num_color_attachments;
    }
    p.base_layer = info.num_layers > 1 ? info.base_layer : 0;
    p.layers = info.num_layers;
    p.subpasses = info.num_subpasses;
    for (unsigned i = 0; i < info.num_subpasses; i++) p.subpass[i] = info.subpasses[i];
    std::lock_guard<std::mutex> hold(pw.lock);
    pw.passes.emplace(hash, p);
}

void pw_note_shader(const Shader *s) {
    if (pw.shaders.count(s->get_hash())) return;
    std::vector<uint8_t> layout(ResourceLayout::serialization_size());
    if (s->get_layout().serialize(layout.data(), layout.size())) pw.shaders.emplace(s->get_hash(), layout);
}

// the samples of a compatible render pass (its first colour attachment's)
uint32_t pw_pass_samples(uint64_t pass) {
    auto it = pw.passes.find(pass);
    return it == pw.passes.end() ? 0 : std::max(1u, it->second.colour_samples[0]);
}

void pw_on_compile(Device *device, const DeferredPipelineCompile &c, bool compute, int64_t ns, bool sync, bool ok) {
    if (!ok || compute || device != pw.device) return; // (a failed one: one not in the cache, asked for so)
    pw.made++;
    pw.made_at = std::chrono::steady_clock::now();
    if (!sync) {
        pw.ahead_ns += uint64_t(ns);
        return; // (the start's own)
    }
    // a draw waited for it: a hitch
    pw.play_made++;
    pw.play_ns += uint64_t(ns);
    uint64_t was = pw.play_max_ns.load();
    while (uint64_t(ns) > was && !pw.play_max_ns.compare_exchange_weak(was, uint64_t(ns))) {}
    PwEntry e;
    e.hash = c.hash;
    e.subgroup = c.subgroup_size_tag;
    e.state = c.static_state;
    e.potential = c.potential_static_state;
    std::lock_guard<std::mutex> hold(pw.lock);
    const Shader *vs = c.program->get_shader(ShaderStage::Vertex), *fs = c.program->get_shader(ShaderStage::Fragment);
    if (!vs || !fs || !c.compatible_render_pass || !pw.passes.count(c.compatible_render_pass->get_hash())) return;
    e.pass = c.compatible_render_pass->get_hash();
    e.tag = pw_is_present(fs->get_hash()) ? PRESENT_TAG : pw_tag(pw_pass_samples(e.pass));
    if (pw.known.count({ e.tag, e.hash })) return;
    pw_note_shader(vs);
    pw_note_shader(fs);
    e.shader[0] = vs->get_hash();
    e.shader[1] = fs->get_hash();
    e.subpass = c.subpass_index;
    std::memcpy(e.attribs, c.attribs, sizeof e.attribs);
    std::memcpy(e.strides, c.strides, sizeof e.strides);
    std::memcpy(e.rates, c.input_rates, sizeof e.rates);
    pw.known.emplace(e.tag, e.hash);
    pw.entries.push_back(e);
    pw.unshipped++;
    pw.play_new++;
    std::fprintf(stderr, "[nd] a pipeline not on the list was made in play (%.1f ms): kept in the data folder's list\n",
                 double(ns) / 1e6);
}

// ---- The start's work -----------------------------------------------------------------------------------------
// A shader by its hash: the one Granite has, or made from the program's SPIR-V with the layout the list kept
Shader *pw_shader(uint64_t hash) {
    if (Shader *s = pw.device->request_shader_by_hash(hash)) return s;
    auto layout = pw.shaders.find(hash);
    if (layout == pw.shaders.end()) return nullptr;
    for (const Spirv &spv : spirv_all) {
        if (Shader::hash(spv.code, spv.size) != hash) continue;
        ResourceLayout l;
        if (!l.unserialize(layout->second.data(), layout->second.size())) return nullptr;
        return pw.device->request_shader(spv.code, spv.size, &l);
    }
    return nullptr; // (a shader of another build)
}

// A compatible render pass made again from its attachments (images of a pixel, let go after); null if it does not
// come out with the hash it was kept under
const RenderPass *pw_render_pass(uint64_t hash, std::vector<ImageHandle> &images) {
    auto it = pw.passes.find(hash);
    if (it == pw.passes.end()) return nullptr;
    const PwPass &p = it->second;
    if (p.layers > 1) return nullptr;
    auto make = [&](uint32_t format, uint32_t samples, bool lazy) -> const ImageView * {
        ImageCreateInfo info = ImageCreateInfo::render_target(1, 1, VkFormat(format));
        info.samples = VkSampleCountFlagBits(samples ? samples : 1);
        info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (lazy) {
            info.domain = ImageDomain::Transient;
            info.usage = (format_has_depth_or_stencil_aspect(VkFormat(format)) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                                                : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                         VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
        }
        ImageHandle image = pw.device->create_image(info);
        if (!image) return nullptr;
        images.push_back(image);
        return &image->get_view();
    };
    RenderPassInfo info;
    info.num_color_attachments = p.colours;
    for (uint32_t i = 0; i < p.colours; i++) {
        info.color_attachments[i] = make(p.colour_format[i], p.colour_samples[i], (p.lazy >> i) & 1);
        if (!info.color_attachments[i]) return nullptr;
    }
    if (p.depth_format) {
        info.depth_stencil = make(p.depth_format, p.depth_samples, (p.lazy >> p.colours) & 1);
        if (!info.depth_stencil) return nullptr;
    }
    info.base_layer = p.base_layer;
    info.num_layers = p.layers;
    info.num_subpasses = p.subpasses;
    info.subpasses = p.subpasses ? p.subpass : nullptr;
    const RenderPass &rp = CommandBuffer::holyland_render_pass(pw.device, info);
    return rp.get_hash() == hash ? &rp : nullptr;
}

void pw_run(std::vector<DeferredPipelineCompile> jobs, unsigned threads) {
    // first those the pipeline cache has (made without compiling: at once), then the rest
    std::vector<size_t> missing;
    std::mutex missing_lock;
    std::atomic<size_t> next{0};
    auto pass = [&](bool build, const std::vector<size_t> &which) {
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; t++) {
            pool.emplace_back([&, build] {
                Util::register_thread_index(0); // (no command buffers on these threads: only Granite's log reads it)
                for (;;) {
                    const size_t i = next++;
                    if (i >= which.size() || pw.stop.load()) break;
                    DeferredPipelineCompile &j = jobs[which[i]];
                    if (j.program->get_pipeline(j.hash).pipeline != VK_NULL_HANDLE) {
                        build ? pw.done++ : pw.ready++; // (a draw made it meanwhile)
                        continue;
                    }
                    const auto mode = build ? CommandBuffer::CompileMode::AsyncThread : CommandBuffer::CompileMode::FailOnCompileRequired;
                    const Pipeline p = CommandBuffer::build_graphics_pipeline(pw.device, j, mode);
                    if (build) {
                        pw.done++;
                    } else if (p.pipeline != VK_NULL_HANDLE) {
                        pw.ready++;
                    } else {
                        std::lock_guard<std::mutex> hold(missing_lock);
                        missing.push_back(which[i]);
                    }
                }
            });
        }
        for (auto &t : pool) t.join();
    };
    std::vector<size_t> all(jobs.size());
    for (size_t i = 0; i < all.size(); i++) all[i] = i;
    pass(false, all);
    std::sort(missing.begin(), missing.end()); // (in the list's order)
    pw.total += int(missing.size());
    if (!missing.empty()) {
        const auto start = std::chrono::steady_clock::now();
        std::fprintf(stderr, "[nd] building shaders: %zu of %zu pipelines are not in the pipeline cache\n", missing.size(),
                     jobs.size());
        next = 0;
        pass(true, missing);
        pw.seconds_build += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::fprintf(stderr, "[nd] building shaders: %d made in %.2f s\n", pw.done.load(), pw.seconds_build);
    }
    pw.running--;
}

} // namespace

void pipeline_warm_open(Device &device, const std::string &data_dir, bool warm) {
    pw.device = &device;
    pw.warm = warm;
    if (const char *e = std::getenv("ELTALE_WARM")) pw.warm = std::atoi(e) != 0;
    const std::string folder = data_dir.empty() ? std::string() : data_dir + "/";
    pw.cache_path = folder + "native_pipelines.bin";
    pw.list_path = folder + "native_pipeline_list.txt";
    // the pipeline cache (made again empty if the file is not this driver's, or damaged)
    std::vector<uint8_t> data;
    if (FILE *f = std::fopen(pw.cache_path.c_str(), "rb")) {
        uint8_t buffer[65536];
        size_t n;
        while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) data.insert(data.end(), buffer, buffer + n);
        std::fclose(f);
    }
    device.init_pipeline_cache(data.empty() ? nullptr : data.data(), data.size());
    pw.cache_saved_size = device.get_pipeline_cache_size();
    holyland_on_compile = pw_on_compile;
    holyland_on_render_pass = pw_on_render_pass;
    // the lists: the shipped one, then the data folder's
    std::lock_guard<std::mutex> hold(pw.lock);
    for (const char *line : nd_pipeline_list) pw_read_line(line, true);
    pw.saved = pw.unshipped = 0;
    if (FILE *f = std::fopen(pw.list_path.c_str(), "rb")) {
        std::string text;
        char buffer[65536];
        size_t n;
        while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) text.append(buffer, n);
        std::fclose(f);
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(at, end - at);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pw_read_line(line, false);
            at = end + 1;
        }
    }
    pw.saved = pw.unshipped;
}

void pipeline_warm_start(uint32_t samples) {
    if (!pw.device || !pw.warm) return;
    std::vector<DeferredPipelineCompile> jobs;
    {
        std::lock_guard<std::mutex> hold(pw.lock);
        const bool first = pw.started.empty(); // (the presenter's, tagged "p", with the first count started)
        if (!pw.started.insert(samples).second) return;
        pw_starting = true;
        struct Done {
            ~Done() { pw_starting = false; }
        } done;
        // this sample count's pipelines, made again from their lines and checked by their hashes
        const std::string tag = pw_tag(samples);
        std::set<uint64_t> taken;
        std::vector<ImageHandle> images;
        const VkPipelineCache cache = CommandBuffer::holyland_pipeline_cache(pw.device);
        for (const PwEntry &e : pw.entries) {
            if ((e.tag != tag && !(first && e.tag == PRESENT_TAG)) || !taken.insert(e.hash).second) continue;
            DeferredPipelineCompile j;
            std::memset(&j, 0, sizeof j);
            Shader *vs = pw_shader(e.shader[0]), *fs = pw_shader(e.shader[1]);
            Program *program = vs && fs ? pw.device->request_program(vs, fs) : nullptr;
            j.compatible_render_pass = program ? pw_render_pass(e.pass, images) : nullptr;
            if (!program || !j.compatible_render_pass) {
                pw.stale++;
                continue;
            }
            j.program = program;
            j.layout = program->get_pipeline_layout();
            j.static_state = e.state;
            j.potential_static_state = e.potential;
            std::memcpy(j.attribs, e.attribs, sizeof e.attribs);
            std::memcpy(j.strides, e.strides, sizeof e.strides);
            std::memcpy(j.input_rates, e.rates, sizeof e.rates);
            j.subpass_index = e.subpass;
            j.cache = cache;
            j.subgroup_size_tag = e.subgroup;
            CommandBuffer::holyland_rehash(j, false);
            if (j.hash != e.hash) {
                pw.stale++; // (Granite would not look it up so: another build's)
                continue;
            }
            jobs.push_back(j);
        }
    }
    if (jobs.empty()) return;
    const unsigned hw = std::thread::hardware_concurrency();
    unsigned threads = hw > 6 ? std::min(hw - 4, 8u) : 2;
    if (const char *t = std::getenv("ELTALE_WARM_THREADS")) threads = unsigned(std::max(1, std::atoi(t)));
    std::fprintf(stderr, "[nd] pipelines made ahead for MSAA %ux: %zu on %u threads\n", samples, jobs.size(), threads);
    pw.running++;
    pw.threads.emplace_back(pw_run, std::move(jobs), threads);
}

void pipeline_warm_wait() {
    if (!pw.device || pw.running.load() <= 0) return;
    const auto start = std::chrono::steady_clock::now();
    while (pw.running.load() > 0 && std::chrono::steady_clock::now() - start < std::chrono::seconds(5))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    pw.waited_ms += ms;
    std::fprintf(stderr, "[nd] waited %.0f ms for the pipelines made ahead\n", ms);
}

void pipeline_warm_tick() {
    if (!pw.device) return;
    const uint64_t made = pw.made.load();
    if (made == pw.made_saved) return;
    if (std::chrono::steady_clock::now() - pw.made_at < std::chrono::seconds(2)) return;
    pw.made_saved = made;
    pw_save_cache();
    pw_save_list();
}

void pipeline_warm_page() {
    if (!pw.device) return;
    if (pw.running.load() > 0)
        ImGui::TextDisabled("Building shaders: %d of %d (and %d from the cache)", pw.done.load(), pw.total.load(), pw.ready.load());
    else
        ImGui::TextDisabled("Shaders made ahead: %d built, %d from the cache; made in play: %llu (%.0f ms)", pw.done.load(),
                            pw.ready.load(), (unsigned long long)pw.play_made.load(), double(pw.play_ns.load()) / 1e6);
}

void pipeline_warm_close() {
    if (!pw.device) return;
    pw.stop = true;
    for (std::thread &t : pw.threads)
        if (t.joinable()) t.join();
    pw.threads.clear();
    pw_save_cache();
    pw_save_list();
    std::printf("P1_PIPELINES ahead=%d cache=%d stale=%d ahead_ms=%.1f build_s=%.2f waited_ms=%.0f made_in_play=%llu "
                "play_ms=%.1f play_max_ms=%.1f new=%llu\n",
                pw.done.load(), pw.ready.load(), pw.stale.load(), double(pw.ahead_ns.load()) / 1e6, pw.seconds_build, pw.waited_ms,
                (unsigned long long)pw.play_made.load(), double(pw.play_ns.load()) / 1e6, double(pw.play_max_ns.load()) / 1e6,
                (unsigned long long)pw.play_new.load());
    std::fflush(stdout);
    holyland_on_compile = nullptr;
    holyland_on_render_pass = nullptr;
    pw.device = nullptr;
}

} // namespace render
