#version 450
#extension GL_GOOGLE_include_directive : require
// The native renderer's vertex stage (docs/renderer/NATIVE_DRAW.md): what F3DEX 1.23 does to a vertex
// at a VTX, per draw: the matrix, lighting (ambient + 3 directional lights, in object space, as the RSP
// transforms them), texgen from the LookAt, the fog factor into the shade alpha, the TEXTURE scale.
// Depth is reversed and infinite (D32F): z_out = the near plane's w, so z_out / w = near / w; the far
// plane is the fragment stage's (v_far), the near plane the GPU's.
// The fog factor goes out two ways: per vertex, as the RSP makes it (OPT_N64_FOG), and as z and w for a
// factor per pixel (modern).
#include "nd_common.h"

layout(location = 0) out vec4 v_shade;
layout(location = 1) out vec2 v_tc;
layout(location = 2) noperspective out vec2 v_tc_affine;
layout(location = 3) out float v_far;
layout(location = 4) out vec2 v_zw;

float s8(uint c, int shift) {
    return float(int(c << (24 - shift)) >> 24);
}

void main() {
    Vertex v = vertices[gl_VertexIndex];
    Draw d = draws[pc.draw];
    vec4 clip = d.mvp * vec4(v.x, v.y, v.z, 1.0);

    vec4 shade = unpackUnorm4x8(v.c);
    vec3 n = vec3(s8(v.c, 0), s8(v.c, 8), s8(v.c, 16)) / 128.0;
    if ((v.flags & RDV_LIGHT) != 0u) {
        vec3 col = d.ambient.rgb;
        for (int i = 0; i < 3; i++) col += max(dot(n, d.light_dir[i].xyz), 0.0) * d.light_col[i].rgb;
        shade.rgb = min(col, vec3(1.0));
    }
    // the RSP's fog factor: z / |w| (so a vertex behind the camera is fully fogged; the clipper then
    // interpolates it like the colour). Measured against the reference, perspective-correct interpolation
    // of the shade fits the RDP's pictures best (its clipped polygons' new vertices).
    float fog = clip.w != 0.0 ? clamp(clip.z / abs(clip.w) * d.fog.x + d.fog.y, 0.0, 255.0) / 255.0 : 1.0;
    if ((v.flags & RDV_FOG) != 0u) shade.a = fog;

    vec2 tc = vec2(v.s, v.t);
    if ((v.flags & RDV_TEXGEN) != 0u) {
        vec2 g = vec2(dot(n, d.lookat_x.xyz), dot(n, d.lookat_y.xyz));
        if ((v.flags & RDV_TEXGEN_LIN) != 0u) g = acos(clamp(g, -1.0, 1.0)) * (1024.0 / 3.14159265);
        else g = (g * 0.5 + 0.5) * 1024.0;
        tc = g * vec2(float(v.scale & 0xFFFFu), float(v.scale >> 16)) / 65536.0;
    }

    v_shade = shade;
    v_zw = clip.zw;
    v_tc = tc;
    v_tc_affine = tc;
    v_far = clip.w - clip.z;
    gl_Position = vec4(clip.x, -clip.y, d.fog.z, clip.w);
}
