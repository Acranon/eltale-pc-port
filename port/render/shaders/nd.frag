#version 450
#extension GL_GOOGLE_include_directive : require
// The native renderer's pixel stage (docs/renderer/NATIVE_DRAW.md): the RDP's texture unit (tiles with
// shift, clamp, mirror and mask, odd spans; CI palettes looked up per frame; N64 three-point or
// four-point filtering), the colour combiner (one or two cycles, in the RDP's integer arithmetic), the
// blender's fog cycle (FOG_SHADE_A) and its surface cycle (a blend with the picture is the GPU's blend
// stage, set up by nd_renderer.cpp from the same othermode bits), alpha compare and CVG x alpha.
#include "nd_common.h"

layout(location = 0) in vec4 v_shade;
layout(location = 1) in vec2 v_tc;
layout(location = 2) noperspective in vec2 v_tc_affine;
layout(location = 3) in float v_far;
layout(location = 4) in vec2 v_zw;
layout(location = 5) in vec3 v_world;
layout(location = 6) in vec3 v_normal;
layout(location = 0) out vec4 o_color;

// The depth buffer as it was before this decal draw (nd_renderer.cpp copies it when a decal comes after
// a depth write): the RDP's decal test is two-sided, so it is done here (OPT_DECAL). With MSAA (nd_ms.frag:
// ND_MSAA) it is multisampled and the test reads its first sample (the draw is shaded once a pixel).
#ifdef ND_MSAA
layout(set = 0, binding = 5) uniform sampler2DMS depth_before;
#define DEPTH_AT(p) texelFetch(depth_before, p, 0).r
#define DEPTH_SIZE() textureSize(depth_before)
#else
layout(set = 0, binding = 5) uniform sampler2D depth_before;
#define DEPTH_AT(p) texelFetch(depth_before, p, 0).r
#define DEPTH_SIZE() textureSize(depth_before, 0)
#endif

// The picture's scale (its pixels per N64 pixel): from the depth's height (the width grows with widescreen)
float picture_scale() {
    return float(DEPTH_SIZE().y) / 240.0;
}

// The RDP's Z_MODE decal: pass where |z - z in memory| <= dz max, dz = |dz/dx| + |dz/dy| of the pixel
// (the new one's, and the one stored with the memory's z) rounded up to a power of two, in the RDP's
// screen-z units. Depth here is near/w and the N64's screen z is affine in 1/w, so the test is the same
// in either after one scale: d.fog.w = N64 screen-z units per unit of depth (nd_renderer.cpp). The
// tolerance is in N64 pixels (a bigger picture keeps the N64's window).
bool decal_pass(Draw d, float dz_new) {
    ivec2 p = ivec2(gl_FragCoord.xy), size = DEPTH_SIZE();
    float px = picture_scale();
    float z = gl_FragCoord.z, zm = DEPTH_AT(p);
    float dzn = dz_new * px;
    // the memory's dz: its surface's slope from the neighbours (the smaller side, so an edge counts less)
    ivec2 hi = size - 1;
    float zl = DEPTH_AT(clamp(p - ivec2(1, 0), ivec2(0), hi));
    float zr = DEPTH_AT(clamp(p + ivec2(1, 0), ivec2(0), hi));
    float zu = DEPTH_AT(clamp(p - ivec2(0, 1), ivec2(0), hi));
    float zd = DEPTH_AT(clamp(p + ivec2(0, 1), ivec2(0), hi));
    float dzm = (min(abs(zm - zl), abs(zr - zm)) + min(abs(zm - zu), abs(zd - zm))) * px;
    float dz = max(dzn, dzm) * d.fog.w;
    dz = exp2(ceil(log2(max(dz, 1.0))));
    return abs(z - zm) * d.fog.w <= dz;
}

ivec4 rgba8(uint c) {
    return ivec4(int(c & 255u), int(c >> 8 & 255u), int(c >> 16 & 255u), int(c >> 24));
}

ivec4 palette_entry(uint e, uint tlut) {
    if (tlut == 3u) {
        int i = int(e >> 8 & 255u);
        return ivec4(i, i, i, int(e & 255u));
    }
    int r = int(e >> 11 & 31u), g = int(e >> 6 & 31u), b = int(e >> 1 & 31u);
    return ivec4(r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, (e & 1u) != 0u ? 255 : 0);
}

ivec4 texel(Tile t, int s, int u, uint tlut) {
    uint w = t.wh & 0xFFFFu;
    uint v = texels[t.texels + uint(u) * w + uint(s)];
    if ((t.fmt & 0xFFFFu) == 2u) return palette_entry(palettes[(t.fmt >> 16) * 256u + v], tlut);
    return rgba8(v);
}

// One axis of the texture unit: an S10.5 coordinate to two texel indices and the 5-bit fraction.
void axis(int c, int lo, int hi, int shift, int mask, int mode, out int i0, out int i1, out int frac) {
    c -= lo << 3;
    if (shift < 11) c >>= shift;
    else c <<= (16 - shift);
    int i = c >> 5, j;
    frac = c & 31;
    j = i + 1;
    if ((mode & 2) != 0) {
        int mx = (hi - lo) >> 2;
        if (c < 0) { i = 0; j = 0; frac = 0; }
        else if (i >= mx) { i = mx; j = mx; frac = 0; }
        else if (j > mx) j = mx;
    }
    if (mask != 0) {
        int m = (1 << mask) - 1;
        if ((mode & 1) != 0) {
            i = ((i >> mask) & 1) != 0 ? m - (i & m) : (i & m);
            j = ((j >> mask) & 1) != 0 ? m - (j & m) : (j & m);
        } else {
            i &= m;
            j &= m;
        }
    }
    i0 = i;
    i1 = j;
}

ivec4 sample_tile(Tile t, vec2 tc, uint filt, uint tlut, ivec2 offset) {
    if ((t.fmt & 0xFFFFu) == 0u) return ivec4(0);
    int s = int(floor(tc.x * 32.0)), u = int(floor(tc.y * 32.0));
    int s0, s1, fs, t0, t1, ft;
    axis(s, t.sl + offset.x, t.sh + offset.x, int(t.mode >> 12 & 15u), int(t.mode >> 4 & 15u), int(t.mode & 3u), s0, s1, fs);
    axis(u, t.tl + offset.y, t.th + offset.y, int(t.mode >> 16 & 15u), int(t.mode >> 8 & 15u), int(t.mode >> 2 & 3u), t0, t1, ft);
    ivec4 c00 = texel(t, s0, t0, tlut);
    if (filt == 0u) return c00;
    ivec4 c10 = texel(t, s1, t0, tlut), c01 = texel(t, s0, t1, tlut), c11 = texel(t, s1, t1, tlut);
    if ((pc.options & OPT_THREE_POINT) != 0u) {
        if (fs + ft < 32) return c00 + (((c10 - c00) * fs + (c01 - c00) * ft + 16) >> 5);
        return c11 + (((c01 - c11) * (32 - fs) + (c10 - c11) * (32 - ft) + 16) >> 5);
    }
    ivec4 a = c00 * 32 + (c10 - c00) * fs, b = c01 * 32 + (c11 - c01) * fs;
    return (a * 32 + (b - a) * ft + 512) >> 10;
}

// ---- the combiner ----
ivec4 combined, tex0, tex1, prim, shade, env;
int prim_lod, k4, k5;

ivec3 sel_a(uint s) {
    switch (s) {
    case 0u: return combined.rgb;
    case 1u: return tex0.rgb;
    case 2u: return tex1.rgb;
    case 3u: return prim.rgb;
    case 4u: return shade.rgb;
    case 5u: return env.rgb;
    case 6u: return ivec3(256);
    default: return ivec3(0);
    }
}

ivec3 sel_b(uint s) {
    switch (s) {
    case 0u: return combined.rgb;
    case 1u: return tex0.rgb;
    case 2u: return tex1.rgb;
    case 3u: return prim.rgb;
    case 4u: return shade.rgb;
    case 5u: return env.rgb;
    case 7u: return ivec3(k4);
    default: return ivec3(0);
    }
}

ivec3 sel_c(uint s) {
    switch (s) {
    case 0u: return combined.rgb;
    case 1u: return tex0.rgb;
    case 2u: return tex1.rgb;
    case 3u: return prim.rgb;
    case 4u: return shade.rgb;
    case 5u: return env.rgb;
    case 7u: return ivec3(combined.a);
    case 8u: return ivec3(tex0.a);
    case 9u: return ivec3(tex1.a);
    case 10u: return ivec3(prim.a);
    case 11u: return ivec3(shade.a);
    case 12u: return ivec3(env.a);
    case 14u: return ivec3(prim_lod);
    case 15u: return ivec3(k5);
    default: return ivec3(0);
    }
}

ivec3 sel_d(uint s) {
    switch (s) {
    case 0u: return combined.rgb;
    case 1u: return tex0.rgb;
    case 2u: return tex1.rgb;
    case 3u: return prim.rgb;
    case 4u: return shade.rgb;
    case 5u: return env.rgb;
    case 6u: return ivec3(256);
    default: return ivec3(0);
    }
}

int sel_alpha(uint s) { // A, B and D of the alpha combiner
    switch (s) {
    case 0u: return combined.a;
    case 1u: return tex0.a;
    case 2u: return tex1.a;
    case 3u: return prim.a;
    case 4u: return shade.a;
    case 5u: return env.a;
    case 6u: return 256;
    default: return 0;
    }
}

int sel_alpha_c(uint s) {
    switch (s) {
    case 1u: return tex0.a;
    case 2u: return tex1.a;
    case 3u: return prim.a;
    case 4u: return shade.a;
    case 5u: return env.a;
    case 6u: return prim_lod;
    default: return 0;
    }
}

// (a - b) * c + d in the RDP's arithmetic, then its 9-bit clamp
ivec4 clamp9(ivec4 r) {
    r &= 0x1FF;
    return ivec4(r.x >= 0x180 ? 0 : min(r.x, 255), r.y >= 0x180 ? 0 : min(r.y, 255), r.z >= 0x180 ? 0 : min(r.z, 255),
                 r.w >= 0x180 ? 0 : min(r.w, 255));
}

ivec4 cycle(uint a, uint b, uint c, uint d, uint aa, uint ab, uint ac, uint ad) {
    ivec3 rgb = ((sel_a(a) - sel_b(b)) * sel_c(c) + (sel_d(d) << 8) + 0x80) >> 8;
    int al = ((sel_alpha(aa) - sel_alpha(ab)) * sel_alpha_c(ac) + (sel_alpha(ad) << 8) + 0x80) >> 8;
    return clamp9(ivec4(rgb, al));
}

int to8(float v) { return int(clamp(v, 0.0, 1.0) * 255.0 + 0.5); }

// the blender's colour inputs P and M: 0 the pixel, 2 the blend colour, 3 the fog colour (1 = memory: the GPU's)
ivec3 blend_input(uint s, ivec3 pixel, ivec4 blendc, ivec4 fogc) {
    return s == 2u ? blendc.rgb : s == 3u ? fogc.rgb : pixel;
}

int blend_a(uint s, int pixel_a, ivec4 fogc) {
    return s == 0u ? pixel_a : s == 1u ? fogc.a : s == 2u ? shade.a : 0;
}

// one blender cycle without the picture: (P * a + M * (b + 1)) >> 5 with 5-bit a and b
ivec3 blend_cycle(uint p, uint ma, uint m, uint mb, ivec3 pixel, int pixel_a, ivec4 blendc, ivec4 fogc) {
    int a = blend_a(ma, pixel_a, fogc) >> 3;
    int b = mb == 0u ? (~(a << 3) >> 3) & 31 : mb == 3u ? 0 : 31;
    if (mb == 3u) return (blend_input(p, pixel, blendc, fogc) * a) >> 5;
    return (blend_input(p, pixel, blendc, fogc) * a + blend_input(m, pixel, blendc, fogc) * (b + 1)) >> 5;
}

void main() {
    Material m = materials[pc.material];
    Draw d = draws[pc.draw];
    float dz_new = abs(dFdx(gl_FragCoord.z)) + abs(dFdy(gl_FragCoord.z)); // before any discard
    if ((pc.options & OPT_FAR_CLIP) != 0u && v_far < 0.0) discard;
    if ((pc.options & OPT_DECAL) != 0u && !decal_pass(d, dz_new)) discard;

    uint oh = m.othermode_h, ol = m.othermode_l;
    bool two = (oh >> 20 & 3u) == 1u;
    uint tlut = oh >> 14 & 3u, filt = oh >> 12 & 3u;
    vec2 tc = (oh >> 19 & 1u) != 0u ? v_tc : v_tc_affine;
    // R7: a 2D rectangle in a bigger picture samples its N64 pixel's corner (the texel the 1x picture has there):
    // text and pictures stay crisp, without the filter's blend with a neighbouring (often keyed-out) texel
    if ((pc.options & OPT_RECT) != 0u) {
        int sc = int(picture_scale() + 0.5);
        if (sc > 1) {
            ivec2 k = ivec2(gl_FragCoord.xy) % sc;
            tc -= dFdx(tc) * float(k.x) + dFdy(tc) * float(k.y);
        }
    }

    env = rgba8((m.own & 2u) != 0u ? m.env : d.colors.x);
    prim = rgba8((m.own & 1u) != 0u ? m.prim : d.colors.y);
    ivec4 fogc = rgba8((m.own & 8u) != 0u ? m.fog : d.colors.z);
    ivec4 blendc = rgba8((m.own & 4u) != 0u ? m.blend : d.colors.w);
    // The fog factor in the shade's alpha (G_FOG): the RSP's per vertex (N64), or per pixel (modern)
    vec4 sh = v_shade;
    if ((m.geometry & 0x10000u) != 0u && (pc.options & OPT_N64_FOG) == 0u)
        sh.a = clamp(v_zw.x / v_zw.y * d.fog.x + d.fog.y, 0.0, 255.0) / 255.0;
    // v11, an overlay's see-through surface (RDV_RIM: a normal came with it): the shade's alpha by how edge-on it is
    // to this picture's eye (light_dir[0], nd_renderer.cpp; light_col[0]: the floor, the power and the soft outline,
    // render.h EL_RD_RIM_*). |n.v|: its back faces get the same rim as its front
    if (dot(v_normal, v_normal) > 0.0) {
        float ndv = abs(dot(normalize(v_normal), normalize(d.light_dir[0].xyz - v_world)));
        vec3 k = d.light_col[0].xyz;
        sh.a *= (k.x + (1.0 - k.x) * pow(1.0 - ndv, k.y)) * smoothstep(0.0, k.z, ndv);
    }
    shade = ivec4(to8(sh.r), to8(sh.g), to8(sh.b), to8(sh.a));
    prim_lod = int(m.misc & 255u);
    k4 = int(m.misc >> 8 & 255u);
    k5 = int(m.misc >> 16 & 255u);

    tex0 = ivec4(0);
    tex1 = ivec4(0);
    if ((m.flags & 1u) != 0u) {
        ivec2 off = (m.flags & 2u) != 0u ? ivec2(d.tile_offset.xy) : ivec2(0);
        tex0 = sample_tile(m.tile[0], tc, filt, tlut, off);
        tex1 = sample_tile(m.tile[1], tc, filt, tlut, ivec2(0));
    }
    ivec4 texel0_seen = tex0;

    uint w0 = m.combine0, w1 = m.combine1;
    combined = ivec4(0);
    ivec4 c = cycle(w0 >> 20 & 15u, w1 >> 28 & 15u, w0 >> 15 & 31u, w1 >> 15 & 7u, w0 >> 12 & 7u, w1 >> 12 & 7u, w0 >> 9 & 7u,
                    w1 >> 9 & 7u);
    if (two) {
        combined = c;
        ivec4 t = tex0; // the second cycle sees the next texel as TEXEL0
        tex0 = tex1;
        tex1 = t;
        c = cycle(w0 >> 5 & 15u, w1 >> 24 & 15u, w0 & 31u, w1 >> 6 & 7u, w1 >> 21 & 7u, w1 >> 3 & 7u, w1 >> 18 & 7u, w1 & 7u);
    }

    // alpha compare (threshold against the blend colour's alpha) and coverage x alpha (cut-outs)
    if ((ol & 3u) == 1u && c.a < blendc.a) discard;
    if ((ol & 0x1000u) != 0u && c.a < 32) discard;

    ivec3 pixel = c.rgb;
    int pixel_a = c.a;
    // the first blender cycle (2-cycle): FOG_SHADE_A and the like, without the picture
    if (two) {
        uint p = ol >> 30 & 3u, ma = ol >> 26 & 3u, mm = ol >> 22 & 3u, mb = ol >> 18 & 3u;
        if (p != 1u && mm != 1u) pixel = blend_cycle(p, ma, mm, mb, pixel, pixel_a, blendc, fogc);
    }
    // the last cycle: without FORCE_BL the RDP writes P; with it, a blend (with the picture: the GPU's)
    uint p = two ? ol >> 28 & 3u : ol >> 30 & 3u, ma = two ? ol >> 24 & 3u : ol >> 26 & 3u;
    uint mm = two ? ol >> 20 & 3u : ol >> 22 & 3u, mb = two ? ol >> 16 & 3u : ol >> 18 & 3u;
    float out_a = 1.0;
    bool blended = false;
    if ((ol & 0x4000u) != 0u) {
        if (mm == 1u) {
            pixel = blend_input(p, pixel, blendc, fogc);
            out_a = float(clamp(blend_a(ma, pixel_a, fogc), 0, 255)) / 255.0;
            blended = true;
        } else if (p != 1u) {
            pixel = blend_cycle(p, ma, mm, mb, pixel, pixel_a, blendc, fogc);
        }
    } else {
        pixel = blend_input(p == 1u ? 0u : p, pixel, blendc, fogc);
    }
    pixel = clamp(pixel, 0, 255);
    // R7, the N64 dither (Classic): the RDP writes 5 bits a channel, rounding up where the 3 bits it drops are
    // over its magic square's value at the N64 pixel. A draw the GPU blends with the picture is left smooth
    // (the RDP dithers after the blend, which happens after this stage here)
    if ((pc.options & OPT_DITHER) != 0u && !blended) {
        const int magic[16] = int[16](0, 6, 1, 7, 4, 2, 5, 3, 1, 7, 0, 6, 5, 3, 4, 2);
        ivec2 n64 = ivec2(gl_FragCoord.xy / picture_scale());
        int dv = magic[(n64.y & 3) * 4 + (n64.x & 3)];
        ivec3 c5 = min((pixel >> 3) + ivec3(greaterThan(pixel & 7, ivec3(dv))), ivec3(31));
        pixel = c5 << 3 | c5 >> 2;
    }
    o_color = vec4(vec3(pixel) / 255.0, out_a);
    // ELTALE_ND_VIEW (a test)
    uint view = pc.pad;
    if (view == 1u) o_color = vec4(v_shade.rgb, 1.0);
    else if (view == 2u) o_color = vec4(vec3(texel0_seen.rgb) / 255.0, 1.0);
    else if (view == 3u) o_color = vec4(vec3(sh.a), 1.0);
    else if (view == 4u) o_color = vec4(vec3(c.rgb) / 255.0, 1.0);
}
