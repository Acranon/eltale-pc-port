// The native renderer's shader interface (port/render/nd_renderer.cpp keeps the C++ side in step).

layout(push_constant) uniform Push {
    uint draw;      // index into draws
    uint material;  // index into materials
    uint options;   // OPT_*
    uint pad;
} pc;

const uint OPT_THREE_POINT = 1u;  // N64 three-point filtering (else four-point bilinear)
const uint OPT_FAR_CLIP = 2u;     // the draw crosses the far plane: cut there
const uint OPT_N64_FOG = 4u;      // the fog factor per vertex as the RSP makes it (else per pixel)
const uint OPT_DECAL = 8u;        // Z_MODE decal: the RDP's two-sided test against depth_before
const uint OPT_DITHER = 16u;      // the RDP's dither: 5 bits a channel with its magic square (R7, Classic)
const uint OPT_RECT = 32u;        // a 2D rectangle (R7: sampled at its N64 pixel's corner in a bigger picture)

const uint RDV_LIGHT = 1u, RDV_TEXGEN = 2u, RDV_TEXGEN_LIN = 4u, RDV_FOG = 8u, RDV_SHADE = 16u;
const uint RDV_RIM = 32u; // v11, an overlay's see-through surface: s, t, scale (a float) its world normal (nd.frag)

struct Vertex {
    float x, y, z, s, t;
    uint c;       // RGBA8, or a normal (s8 x y z) and the alpha
    uint flags;   // RDV_*
    uint scale;   // TEXTURE scale s | t << 16
};

struct Tile {
    uint texels;  // offset into texels
    uint wh;      // width | height << 16
    uint fmt;     // format (1 RGBA, 2 CI) | palette << 16
    uint mode;    // cms | cmt << 2 | masks << 4 | maskt << 8 | shifts << 12 | shiftt << 16
    int sl, tl, sh, th;
};

struct Material {
    uint combine0, combine1, othermode_h, othermode_l;
    uint geometry, own, prim, env;
    uint blend, fog, misc, flags;  // misc: prim LOD frac | k4 << 8 | k5 << 16
    Tile tile[2];
};

struct Draw {
    mat4 mvp;           // model * projection stack (N64 row-vector layout: mvp * v is v * M)
    vec4 light_dir[3];  // object space, normalized
    vec4 light_col[3];  // 0..1
    vec4 ambient;
    vec4 lookat_x, lookat_y; // object space, normalized
    uvec4 colors;       // env, prim, fog, blend (RGBA8)
    vec4 fog;           // fm, fo, the near plane's w (the depth), N64 screen-z units per unit of depth
    vec4 tile_offset;   // s, t (10.2) for EL_RDM_TILE_SCROLL tiles; z, w: the draw distance's A and k (nd.vert)
};

layout(std430, set = 0, binding = 0) readonly buffer Vertices { Vertex vertices[]; };
layout(std430, set = 0, binding = 1) readonly buffer Draws { Draw draws[]; };
layout(std430, set = 0, binding = 2) readonly buffer Materials { Material materials[]; };
layout(std430, set = 0, binding = 3) readonly buffer Texels { uint texels[]; };
layout(std430, set = 0, binding = 4) readonly buffer Palettes { uint palettes[]; };
