// eltale-rdpref: the exact, offline pixel reference for a dumped frame.
//
// A frame image (out/frame_N.bin from ELTALE_FRAME_IMAGE=1 ELTALE_DUMP_IMAGE=N: 8 MiB of N64 RDRAM,
// big-endian, then the root display list's address) is drawn the way the N64 draws it:
//   - port/rsp runs the ROM's own rspboot and F3DEX 1.23 (text and data read from the user's ROM)
//     over the image, as osSpTaskLoad/osSpTaskStartGo would start the game's graphics task;
//   - F3DEX (the FIFO build) DMAs its RDP commands into the task's output buffer in RDRAM and
//     moves DPC_START/DPC_END; each command is handed to paraLLEl-RDP (Vulkan compute, headless);
//   - the colour image the frame drew into is read back from paraLLEl-RDP's RDRAM and written as
//     a 320x240 PNG; --vi also writes paraLLEl-RDP's VI scan-out (the game's NTSC LAN1 mode with
//     its special features: gamma off, divot on, dither filter on).
//
// usage: eltale-rdpref --rom ROM.z64 [--out DIR] [--vi] [--dump-rdp] [--list FILE] frame_N.bin...
// Prints one line per frame:
//   RDPREF frame=N fb=XXXXXXXX rdp_cmds=.. tris=.. rects=.. hash=<fnv1a-64 of the 320x240 RGBA5551
//   picture> [vi=WxH vi_hash=..]
#include "rdp_device.hpp"
#include "context.hpp"
#include "device.hpp"
#include "thread_id.hpp"

extern "C" {
#include "rsp.h"
}

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t RDRAM_BYTES = 8u << 20;

// The game's graphics task (Gfx_InitFrameTask, game/core/main.c) and where its pieces sit in the ROM.
constexpr uint32_t ROM_RSPBOOT = 0x4C580, RSPBOOT_SIZE = 0xD0;
constexpr uint32_t ROM_F3DEX_TEXT = 0x4C650, F3DEX_TEXT_SIZE = 0x4DA80 - 0x4C650;
constexpr uint32_t ROM_F3DEX_DATA = 0x70E60, F3DEX_DATA_SIZE = 0x800;
constexpr uint32_t RSPBOOT = 0x0004B980, UCODE = 0x0004BA50, UCODE_DATA = 0x00070260;
constexpr uint32_t DRAM_STACK = 0x00300000, DRAM_STACK_SIZE = 0x400;
constexpr uint32_t OUTPUT_BUFF = 0x0031C5A0, OUTPUT_BUFF_END = 0x003245A0;
constexpr uint32_t YIELD_DATA = 0x00300400, YIELD_DATA_SIZE = 0xC00;

uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// ---- the RSP's view: the frame image, and the RDP command FIFO ----

struct Frame {
    std::vector<uint8_t> image; // 8 MiB big-endian
    uint32_t root = 0;
    // DPC registers
    uint32_t dpc_start = 0, dpc_end = 0, dpc_current = 0;
    bool xbus = false;
    // how the commands came: DPC_END writes, any from DMEM (XBUS), the RDRAM range they came from
    unsigned flushes = 0;
    bool xbus_used = false;
    uint32_t lo = 0xFFFFFFFF, hi = 0;
    Rsp *rsp = nullptr;
    // the RDP command stream, as 32-bit words, in order
    std::vector<uint32_t> rdp;
    std::vector<uint32_t> pending; // words of a command not complete yet
};

uint8_t dram_read(void *ctx, uint32_t a) {
    auto *f = static_cast<Frame *>(ctx);
    a &= 0xFFFFFF;
    return a < RDRAM_BYTES ? f->image[a] : 0;
}

void dram_write(void *ctx, uint32_t a, uint8_t v) {
    auto *f = static_cast<Frame *>(ctx);
    a &= 0xFFFFFF;
    if (a < RDRAM_BYTES) f->image[a] = v;
}

// The RDP takes the words between DPC_CURRENT and DPC_END at once (it is never busy), from RDRAM, or
// from DMEM when the microcode set XBUS.
void rdp_take(Frame *f) {
    for (uint32_t a = f->dpc_current & ~7u; a < (f->dpc_end & ~7u); a += 4) {
        uint32_t w;
        if (f->xbus) w = Rsp_LoadDmem32(f->rsp, a & 0xFFF);
        else w = (a & 0xFFFFFF) + 4 <= RDRAM_BYTES ? be32(&f->image[a & 0xFFFFFF]) : 0;
        f->rdp.push_back(w);
    }
    f->dpc_current = f->dpc_end;
}

uint32_t dpc_read(void *ctx, int reg) {
    auto *f = static_cast<Frame *>(ctx);
    switch (reg) {
    case 0: return f->dpc_start;
    case 1: return f->dpc_end;
    case 2: return f->dpc_current;
    case 3: return f->xbus ? 1u : 0u; // DPC_STATUS: idle, no START/END pending, not frozen
    default: return 0;                // clock, buffer busy, pipe busy, TMEM busy: all 0
    }
}

void dpc_write(void *ctx, int reg, uint32_t v) {
    auto *f = static_cast<Frame *>(ctx);
    switch (reg) {
    case 0: f->dpc_start = f->dpc_current = v & 0xFFFFF8; break;
    case 1:
        f->dpc_end = v & 0xFFFFF8;
        f->flushes++;
        if (f->xbus) f->xbus_used = true;
        else if (f->dpc_end > f->dpc_current) {
            if (f->dpc_current < f->lo) f->lo = f->dpc_current;
            if (f->dpc_end > f->hi) f->hi = f->dpc_end;
        }
        rdp_take(f);
        break;
    case 3:
        if (v & 1) f->xbus = false; // CLR_XBUS_DMEM_DMA
        if (v & 2) f->xbus = true;  // SET_XBUS_DMEM_DMA
        break;
    default: break;
    }
}

[[noreturn]] void rsp_fatal(const char *what) {
    fprintf(stderr, "eltale-rdpref: %s\n", what);
    exit(1);
}

// RDP command lengths in 64-bit words (triangles by their shade/texture/Z bits; texture rectangles 2)
const unsigned cmd_len[64] = {
    1, 1, 1, 1, 1, 1, 1, 1, 4, 6, 12, 14, 12, 14, 20, 22,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};

bool read_file(const char *path, std::vector<uint8_t> &out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size_t(n));
    bool ok = fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

// ---- PNG (stored deflate blocks: no zlib needed) ----

uint32_t crc_table[256];
void crc_init() {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
uint32_t crc(const uint8_t *p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

void put32(std::vector<uint8_t> &v, uint32_t x) {
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
}

void chunk(FILE *f, const char *type, const std::vector<uint8_t> &data) {
    std::vector<uint8_t> b;
    put32(b, uint32_t(data.size()));
    b.insert(b.end(), type, type + 4);
    b.insert(b.end(), data.begin(), data.end());
    put32(b, crc(b.data() + 4, b.size() - 4) ^ 0xFFFFFFFFu);
    fwrite(b.data(), 1, b.size(), f);
}

bool write_png(const std::string &path, unsigned w, unsigned h, const std::vector<uint8_t> &rgb) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    put32(ihdr, w); put32(ihdr, h);
    ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(f, "IHDR", ihdr);
    std::vector<uint8_t> raw;
    raw.reserve((w * 3 + 1) * h);
    for (unsigned y = 0; y < h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + y * w * 3, rgb.begin() + (y + 1) * w * 3);
    }
    std::vector<uint8_t> z = { 0x78, 0x01 };
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw.size() || off == 0;) {
        size_t n = raw.size() - off < 65535 ? raw.size() - off : 65535;
        z.push_back(off + n == raw.size() ? 1 : 0);
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
        if (n == 0) break;
    }
    put32(z, b << 16 | a);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    return fclose(f) == 0;
}

uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

std::string frame_name(const std::string &path) {
    size_t s = path.find_last_of("/\\");
    std::string base = s == std::string::npos ? path : path.substr(s + 1);
    size_t dot = base.rfind('.');
    return dot == std::string::npos ? base : base.substr(0, dot);
}

} // namespace

int main(int argc, char **argv) {
    const char *rom_path = nullptr, *list_path = nullptr;
    std::string out_dir = ".";
    bool want_vi = false, dump_rdp = false;
    std::vector<std::string> frames;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--rom" && i + 1 < argc) rom_path = argv[++i];
        else if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
        else if (a == "--list" && i + 1 < argc) list_path = argv[++i];
        else if (a == "--vi") want_vi = true;
        else if (a == "--dump-rdp") dump_rdp = true;
        else if (a.size() > 1 && a[0] == '-') {
            fprintf(stderr, "usage: eltale-rdpref --rom ROM.z64 [--out DIR] [--vi] [--dump-rdp] [--list FILE] frame_N.bin...\n");
            return 2;
        } else frames.push_back(a);
    }
    if (list_path) {
        FILE *lf = fopen(list_path, "r");
        if (!lf) { fprintf(stderr, "eltale-rdpref: can't read %s\n", list_path); return 1; }
        char line[1024];
        while (fgets(line, sizeof line, lf)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            if (!s.empty() && s[0] != '#') frames.push_back(s);
        }
        fclose(lf);
    }
    if (!rom_path || frames.empty()) {
        fprintf(stderr, "usage: eltale-rdpref --rom ROM.z64 [--out DIR] [--vi] [--dump-rdp] [--list FILE] frame_N.bin...\n");
        return 2;
    }
    std::vector<uint8_t> rom;
    if (!read_file(rom_path, rom) || rom.size() < 0x100000 || be32(rom.data()) != 0x80371240u) {
        fprintf(stderr, "eltale-rdpref: %s is not a big-endian (.z64) ROM\n", rom_path);
        return 1;
    }
    crc_init();

    // ---- Vulkan, headless: no window, no surface ----
    Util::register_thread_index(0);
    if (!Vulkan::Context::init_loader(nullptr)) { fprintf(stderr, "eltale-rdpref: no Vulkan loader\n"); return 1; }
    Vulkan::Context context;
    context.set_num_thread_indices(1);
    if (!context.init_instance_and_device(nullptr, 0, nullptr, 0)) {
        fprintf(stderr, "eltale-rdpref: no Vulkan device\n");
        return 1;
    }
    Vulkan::Device device;
    device.set_context(context);
    RDP::CommandProcessor rdp(device, nullptr, 0, RDRAM_BYTES, RDRAM_BYTES / 2,
                              RDP::COMMAND_PROCESSOR_FLAG_HOST_VISIBLE_HIDDEN_RDRAM_BIT |
                              RDP::COMMAND_PROCESSOR_FLAG_HOST_VISIBLE_TMEM_BIT);
    if (!rdp.device_is_supported()) { fprintf(stderr, "eltale-rdpref: paraLLEl-RDP can't run on this GPU\n"); return 1; }
    fprintf(stderr, "eltale-rdpref: %s\n", context.get_gpu_props().deviceName);

    int failures = 0;
    for (const std::string &path : frames) {
        Frame f;
        if (!read_file(path.c_str(), f.image) || f.image.size() != RDRAM_BYTES + 4) {
            fprintf(stderr, "eltale-rdpref: %s is not a frame image\n", path.c_str());
            failures++;
            continue;
        }
        f.root = be32(&f.image[RDRAM_BYTES]);
        f.image.resize(RDRAM_BYTES);
        // the boot code and microcode, from the ROM, where the game's task points
        memcpy(&f.image[RSPBOOT], &rom[ROM_RSPBOOT], RSPBOOT_SIZE);
        memcpy(&f.image[UCODE], &rom[ROM_F3DEX_TEXT], F3DEX_TEXT_SIZE);
        memcpy(&f.image[UCODE_DATA], &rom[ROM_F3DEX_DATA], F3DEX_DATA_SIZE);

        // ---- F3DEX on the RSP, started as osSpTaskLoad + osSpTaskStartGo start it ----
        static Rsp rsp;
        memset(&rsp, 0, sizeof rsp);
        rsp.ctx = &f;
        rsp.dram_read = dram_read;
        rsp.dram_write = dram_write;
        rsp.dpc_read = dpc_read;
        rsp.dpc_write = dpc_write;
        rsp.fatal = rsp_fatal;
        f.rsp = &rsp;
        memcpy(rsp.imem, &rom[ROM_RSPBOOT], RSPBOOT_SIZE);
        const uint32_t task[16] = {
            1 /* M_GFXTASK */, 0, RSPBOOT, RSPBOOT_SIZE, UCODE, 0x1000 /* SP_UCODE_SIZE */,
            UCODE_DATA, F3DEX_DATA_SIZE, DRAM_STACK, DRAM_STACK_SIZE, OUTPUT_BUFF, OUTPUT_BUFF_END,
            f.root & 0xFFFFFF, 0x5000 /* the frame list's buffer; F3DEX doesn't read it */,
            YIELD_DATA, YIELD_DATA_SIZE,
        };
        for (int i = 0; i < 16; i++) Rsp_StoreDmem32(&rsp, 0xFC0 + i * 4, task[i]);
        Rsp_Reset(&rsp, 0);
        if (!Rsp_Run(&rsp, 400000000)) {
            fprintf(stderr, "eltale-rdpref: %s: F3DEX did not finish\n", path.c_str());
            failures++;
            continue;
        }

        // ---- the RDP: RDRAM in host-order words, the commands, then read the picture back ----
        auto *rd = static_cast<uint32_t *>(rdp.begin_read_rdram());
        for (uint32_t i = 0; i < RDRAM_BYTES / 4; i++) rd[i] = be32(&f.image[i * 4]);
        rdp.end_write_rdram();
        memset(rdp.begin_read_hidden_rdram(), 0, rdp.get_hidden_rdram_size());
        rdp.end_write_hidden_rdram();
        memset(rdp.get_tmem(), 0, 0x1000);
        rdp.begin_frame_context();

        uint32_t cimg = 0, cimg_w = 320, cimg_fmt = 0;
        unsigned cmds = 0, tris = 0, rects = 0;
        std::vector<uint32_t> &w = f.rdp;
        size_t i = 0;
        while (i + 2 <= w.size()) {
            unsigned op = (w[i] >> 24) & 63, n = cmd_len[op] * 2;
            if (i + n > w.size()) break;
            if (op == 0x3F) { cimg = w[i + 1] & 0xFFFFFF; cimg_w = (w[i] & 0x3FF) + 1; cimg_fmt = (w[i] >> 19) & 0x1F; }
            if (op >= 0x08 && op <= 0x0F) tris++;
            if (op == 0x24 || op == 0x25 || op == 0x36) rects++;
            rdp.enqueue_command(n, &w[i]);
            cmds++;
            i += n;
        }
        if (i != w.size()) fprintf(stderr, "eltale-rdpref: %s: %zu words left over\n", path.c_str(), w.size() - i);
        rdp.idle();

        std::string name = frame_name(path);
        if (dump_rdp) {
            FILE *df = fopen((out_dir + "/" + name + ".rdp").c_str(), "wb");
            if (df) {
                for (uint32_t x : w) { uint8_t b[4] = { uint8_t(x >> 24), uint8_t(x >> 16), uint8_t(x >> 8), uint8_t(x) }; fwrite(b, 1, 4, df); }
                fclose(df);
            }
        }

        // the colour image the frame drew into: 320x240 RGBA5551
        const auto *rr = static_cast<const uint32_t *>(rdp.begin_read_rdram());
        std::vector<uint8_t> px(320 * 240 * 2), rgb(320 * 240 * 3);
        for (unsigned p = 0; p < 320 * 240; p++) {
            uint32_t a = cimg + p * 2;
            uint32_t word = a + 4 <= RDRAM_BYTES ? rr[a >> 2] : 0;
            uint16_t c = (a & 2) ? uint16_t(word) : uint16_t(word >> 16);
            px[p * 2] = uint8_t(c >> 8);
            px[p * 2 + 1] = uint8_t(c);
            uint8_t r5 = c >> 11, g5 = (c >> 6) & 31, b5 = (c >> 1) & 31;
            rgb[p * 3] = uint8_t(r5 << 3 | r5 >> 2);
            rgb[p * 3 + 1] = uint8_t(g5 << 3 | g5 >> 2);
            rgb[p * 3 + 2] = uint8_t(b5 << 3 | b5 >> 2);
        }
        if (cimg_w != 320 || cimg_fmt != 0x02)
            fprintf(stderr, "eltale-rdpref: %s: colour image %06x is %u wide, format %02x\n", path.c_str(), cimg, cimg_w, cimg_fmt);
        write_png(out_dir + "/" + name + ".png", 320, 240, rgb);
        printf("RDPREF frame=%s fb=%08x rdp_cmds=%u tris=%u rects=%u hash=%016llx", name.c_str(), cimg | 0x80000000u, cmds,
               tris, rects, (unsigned long long)fnv1a(px.data(), px.size()));
        if (getenv("ELTALE_RDPREF_FIFO"))
            fprintf(stderr, "RDPREF_FIFO frame=%s dpc_end_writes=%u xbus=%d rdram=%06x..%06x words=%zu\n", name.c_str(),
                    f.flushes, f.xbus_used ? 1 : 0, f.lo, f.hi, w.size());

        if (want_vi) {
            // osViModeNtscLan1 with osViSetSpecialFeatures(0x5A): gamma and gamma dither off, divot
            // and the dither filter on (game/core/main.c; port/gfx/vi.c builds the same registers)
            using VR = RDP::VIRegister;
            rdp.set_vi_register(VR::Control, (0x311E & ~0xCu) | 0x10 | 0x10000);
            rdp.set_vi_register(VR::Origin, cimg + 640);
            rdp.set_vi_register(VR::Width, 320);
            rdp.set_vi_register(VR::Intr, 2);
            rdp.set_vi_register(VR::VCurrentLine, 0);
            rdp.set_vi_register(VR::Timing, 0x03E52239);
            rdp.set_vi_register(VR::VSync, 0x20D);
            rdp.set_vi_register(VR::HSync, 0xC15);
            rdp.set_vi_register(VR::Leap, 0x0C150C15);
            rdp.set_vi_register(VR::HStart, 0x006C02EC);
            rdp.set_vi_register(VR::VStart, 0x002501FF);
            rdp.set_vi_register(VR::VBurst, 0x000E0204);
            rdp.set_vi_register(VR::XScale, 0x200);
            rdp.set_vi_register(VR::YScale, 0x400);
            std::vector<RDP::RGBA> vi;
            unsigned vw = 0, vh = 0;
            RDP::ScanoutOptions opts;
            rdp.scanout_sync(vi, vw, vh, opts);
            std::vector<uint8_t> vrgb(size_t(vw) * vh * 3);
            for (size_t p = 0; p < size_t(vw) * vh; p++) {
                vrgb[p * 3] = vi[p].r;
                vrgb[p * 3 + 1] = vi[p].g;
                vrgb[p * 3 + 2] = vi[p].b;
            }
            // one field of 240 lines: written line-doubled so the picture has the TV's shape
            if (vw && vh == 240) {
                std::vector<uint8_t> tall(size_t(vw) * 480 * 3);
                for (unsigned y = 0; y < 480; y++)
                    memcpy(&tall[size_t(y) * vw * 3], &vrgb[size_t(y / 2) * vw * 3], size_t(vw) * 3);
                write_png(out_dir + "/" + name + ".vi.png", vw, 480, tall);
            } else if (vw && vh) write_png(out_dir + "/" + name + ".vi.png", vw, vh, vrgb);
            printf(" vi=%ux%u vi_hash=%016llx", vw, vh, (unsigned long long)fnv1a(vrgb.data(), vrgb.size()));
        }
        printf("\n");
        fflush(stdout);
    }
    return failures ? 1 : 0;
}
