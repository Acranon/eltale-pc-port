/* The frame image: one graphics task's display list and everything it references, as the
 * N64 would hold them, in an N64-format (big-endian) RDRAM image. A renderer that reads RDRAM
 * the way the RSP and RDP do (paraLLEl-RDP with an LLE RSP, or RT64) draws from this image.
 *
 * The game's data is in two places (docs/P0_STATUS.md, "Address model"):
 * - RDRAM (0x80000000..0x80800000): N64 bytes loaded from the ROM. The image starts as a copy.
 * - native memory: what native code builds (display lists, matrices, vertices, buffers), in host
 *   byte order at host addresses. The walker converts each such object by how the display list
 *   uses it (a list, a matrix, vertices, a texture of a given texel size, a viewport, lights) and
 *   writes it at its N64 address (the symbol's address from the decomp, via
 *   Port_NormalizeDlAddress, or the frame buffers' N64 addresses), rewriting the address words
 *   of native lists to N64 addresses.
 *
 * F3DEX 1.23 commands that carry addresses: MTX, MOVEMEM, VTX, DL, MOVEWORD (segments),
 * SETTIMG (with the following LOADBLOCK/LOADTILE/LOADTLUT for the size), SETCIMG, SETZIMG,
 * RDPHALF_1 + BRANCH_Z. */
#include "port/gfx.h"
#include "game/core/main.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

__attribute__((noreturn)) void Port_HostFatal(const char *what);
uint32_t Port_NormalizeDlAddress(uint32_t word);
extern GfxFrame port_gfx_frames[2];

enum { RDRAM = 0x80000000u, RDRAM_BYTES = 8 << 20, FRAME_N64 = 0x80301000u, FRAME_SIZE = 0xDAD0, FRAME_DATA = 0x58,
       MAX_DEPTH = 18, MAX_COMMANDS = 1 << 16 };

static u8 image[RDRAM_BYTES];
static u32 segments[16];
static PortFrameStats stats;
static struct { u32 addr; int native; u8 fmt, siz; u16 width; } timg;

static int is_rdram(u32 a) { return a >= RDRAM && a < RDRAM + RDRAM_BYTES; }

static u8 cur_op;
static void note_unknown(u32 word) {
    if (stats.unknown < 4) { stats.unknown_words[stats.unknown] = word; stats.unknown_ops[stats.unknown] = cur_op; }
    stats.unknown++;
}

/* A native address's N64 address, or 0. */
static u32 native_to_n64(uintptr_t host) {
    for (unsigned i = 0; i < 2; i++) {
        uintptr_t data = (uintptr_t)&port_gfx_frames[i].data;
        if (host >= data && host < data + sizeof port_gfx_frames[i].data)
            return FRAME_N64 + i * FRAME_SIZE + FRAME_DATA + (u32)(host - data);
    }
    u32 n64 = Port_NormalizeDlAddress((u32)host);
    return is_rdram(n64) ? n64 : 0;
}

/* Where an address word points: *native is set when it is native memory. Returns the N64
 * address (0 if unknown). */
static u32 resolve(u32 word, int *native) {
    *native = 0;
    if (is_rdram(word)) return word;
    if (word < 0x10000000u) { /* segmented, or physical (segment 0) */
        u32 base = segments[word >> 24 & 0xF];
        u32 a = base + (word & 0x00FFFFFF);
        if (a < RDRAM_BYTES) a |= RDRAM;
        if (is_rdram(a)) return a;
        word = a; /* a segment holding a native base */
    }
    *native = 1;
    u32 n64 = native_to_n64(word);
    if (!n64) note_unknown(word);
    return n64;
}

static u8 *at(u32 n64, u32 bytes) {
    if (!is_rdram(n64) || n64 - RDRAM > RDRAM_BYTES - bytes) return NULL;
    return image + (n64 - RDRAM);
}

static void put32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* Copy a native object into the image at its N64 address: `unit` 1 keeps bytes, 2 and 4 write
 * big-endian halfwords/words. The native object is at `host`. */
static void copy_native(uintptr_t host, u32 n64, u32 bytes, int unit) {
    u8 *dst = at(n64, bytes);
    if (!dst) { stats.unknown++; return; }
    const u8 *src = (const u8 *)host;
    if (unit == 1) memcpy(dst, src, bytes);
    else if (unit == 2) for (u32 i = 0; i + 1 < bytes; i += 2) { u16 v; memcpy(&v, src + i, 2); dst[i] = v >> 8; dst[i + 1] = (u8)v; }
    else for (u32 i = 0; i + 3 < bytes; i += 4) { u32 v; memcpy(&v, src + i, 4); put32(dst + i, v); }
    stats.native_bytes += bytes;
}

/* The host address of a native word's target (segment-relative words included). */
static uintptr_t native_host(u32 word) {
    if (word < 0x10000000u) return segments[word >> 24 & 0xF] + (word & 0x00FFFFFF);
    return word;
}

static void walk(u32 word, int depth);

static void ref_data(u32 word, u32 bytes, int unit) {
    int native;
    u32 n64 = resolve(word, &native);
    if (native && n64) copy_native(native_host(word), n64, bytes, unit);
}

static void texture_load(u32 bytes) {
    if (!timg.addr || !bytes) return;
    stats.textures++;
    if (timg.native) {
        u32 n64 = native_to_n64(timg.addr);
        if (n64) copy_native(timg.addr, n64, bytes, timg.siz == 2 ? 2 : timg.siz == 3 ? 4 : 1);
        else stats.unknown++;
    }
}

/* One display list. A native list is rewritten into the image in N64 form. */
static void walk(u32 word, int depth) {
    if (depth > MAX_DEPTH) Port_HostFatal("display lists nested too deep");
    int native;
    u32 n64 = resolve(word, &native);
    if (!n64) return;
    const u8 *src = native ? (const u8 *)native_host(word) : image + (n64 - RDRAM);
    u32 half1 = 0;
    for (u32 i = 0; i < MAX_COMMANDS; i++) {
        u32 w0, w1;
        if (native) { memcpy(&w0, src + i * 8, 4); memcpy(&w1, src + i * 8 + 4, 4); }
        else { const u8 *p = src + i * 8; w0 = (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
               w1 = (u32)p[4] << 24 | p[5] << 16 | p[6] << 8 | p[7]; }
        u8 op = w0 >> 24;
        cur_op = op;
        u32 out1 = w1;
        stats.commands++;
        int ref_native;
        switch (op) {
        case 0x01: ref_data(w1, 64, 4); out1 = resolve(w1, &ref_native); stats.matrices++; break; /* MTX */
        case 0x03: /* MOVEMEM: viewport halfwords, lights/lookat bytes */
            ref_data(w1, w0 & 0xFFFF, (w0 >> 16 & 0xFF) == 0x80 ? 2 : 1);
            out1 = resolve(w1, &ref_native); break;
        case 0x04: { /* VTX */
            u32 n = w0 >> 10 & 0x3F, bytes = n * 16;
            int vn; u32 v = resolve(w1, &vn);
            if (vn && v) {
                const u8 *s = (const u8 *)native_host(w1);
                u8 *d = at(v, bytes);
                if (d) for (u32 k = 0; k < n; k++)
                    for (u32 h = 0; h < 16; h++) {
                        if (h < 12) { u16 x; memcpy(&x, s + k * 16 + (h & ~1u), 2); d[k * 16 + h] = h & 1 ? (u8)x : x >> 8; }
                        else d[k * 16 + h] = s[k * 16 + h];
                    }
                stats.native_bytes += bytes;
            }
            stats.vertices += n;
            out1 = v; break; }
        case 0x06: walk(w1, depth + 1); out1 = resolve(w1, &ref_native); break; /* DL / branch */
        case 0xB4: half1 = w1; break; /* RDPHALF_1 */
        case 0xB0: if (half1) walk(half1, depth + 1); break; /* BRANCH_Z */
        case 0xBC: /* MOVEWORD: segment table entries */
            if ((w0 & 0xFF) == 0x06) {
                u32 seg = (w0 >> 8 & 0xFFFF) / 4 & 0xF;
                segments[seg] = w1;
                if (w1 >= 0x10000000u && !is_rdram(w1)) { u32 n = native_to_n64(w1); out1 = n ? n - RDRAM : 0; }
            }
            break;
        case 0xFD: /* SETTIMG */
            timg.addr = w1; timg.siz = w0 >> 19 & 3; timg.width = (w0 & 0xFFF) + 1;
            { int tn; u32 t = resolve(w1, &tn); timg.native = tn; if (tn) timg.addr = (u32)native_host(w1); out1 = t; }
            break;
        case 0xF3: { /* LOADBLOCK: lrs+1 texels */
            u32 texels = (w1 >> 12 & 0xFFF) + 1;
            texture_load(timg.siz == 0 ? (texels + 1) / 2 : texels << (timg.siz - 1));
            break; }
        case 0xF4: { /* LOADTILE: rows of the image */
            u32 ult = w0 & 0xFFF, lrt = w1 & 0xFFF, lrs = w1 >> 12 & 0xFFF;
            u32 rows = (lrt >> 2) + 1, row_bytes = timg.siz == 0 ? (timg.width + 1) / 2 : timg.width << (timg.siz - 1);
            (void)lrs;
            texture_load(rows * row_bytes);
            (void)ult;
            break; }
        case 0xF0: texture_load(((w1 >> 14 & 0x3FF) + 1) * 2); break; /* LOADTLUT */
        case 0xFE: case 0xFF: out1 = resolve(w1, &ref_native); break; /* SETZIMG, SETCIMG */
        }
        if (is_rdram(out1) && (op == 0x01 || op == 0x03 || op == 0x04 || op == 0x06 || op == 0xFD ||
                               op == 0xFE || op == 0xFF))
            out1 -= RDRAM; /* the RSP and RDP take physical addresses */
        if (native) {
            u8 *d = at(n64 + i * 8, 8);
            if (d) { put32(d, w0); put32(d + 4, out1); }
        }
        if (op == 0xB8) return; /* ENDDL */
        if (op == 0x06 && (w0 >> 16 & 0xFF) == 1) return; /* branch: no return */
    }
    Port_HostFatal("display list without an end");
}

const u8 *Port_BuildFrameImage(u32 dl_word, u32 *root_n64, PortFrameStats *out) {
    memset(&stats, 0, sizeof stats);
    memset(segments, 0, sizeof segments);
    memset(&timg, 0, sizeof timg);
    memcpy(image, (const void *)(uintptr_t)RDRAM, RDRAM_BYTES);
    int native;
    *root_n64 = resolve(dl_word, &native);
    walk(dl_word, 0);
    *out = stats;
    return image;
}

/* ELTALE_FRAME_IMAGE=1: build each frame's image and log what it took. */
void Port_FrameImageTrace(OSTask *task, unsigned frame) {
    PortFrameStats st;
    u32 root;
    Port_BuildFrameImage((u32)(uptr)task->t.data_ptr, &root, &st);
    printf("P1_FRAME_IMAGE frame=%u root=%08x cmds=%u mtx=%u vtx=%u tex=%u native_bytes=%u unknown=%u", frame,
           root, st.commands, st.matrices, st.vertices, st.textures, st.native_bytes, st.unknown);
    for (u32 i = 0; i < st.unknown && i < 4; i++) printf(" %02x:%08x", st.unknown_ops[i], st.unknown_words[i]);
    printf("\n");
}
