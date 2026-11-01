/* render.h: the native renderer's C interface (docs/renderer/NATIVE_DRAW.md).
 *
 * Two sides meet here:
 * - the game's side, in the exe: game/native_draw/ (the adapter the patched draw code calls, and the
 *   model loader that turns a model's static display list into a mesh once);
 * - the renderer, in eltale_host.dll (port/render/): GPU buffers, the recordings, the shaders.
 *
 * The exe gets the renderer's function table from the DLL (ElHost_RenderApi) and calls through it;
 * without a native renderer (RT64, headless runs) the adapter never calls it. Plain C, plain data:
 * every struct is copied by the callee, nothing is kept by pointer.
 *
 * The flow, per game tick (one graphics task):
 *   frame_begin -> camera, lights, colours, fog (state; each draw takes a copy of the current state)
 *              -> draw, draw, ... (in the game's draw order: translucent things are drawn in it)
 *              -> palette (the run-time palettes the drawn meshes use, as they are at the end)
 *              -> frame_end(graphics task number)
 * A finished frame is a recording; the renderer draws the newest one when the game presents (and R6
 * draws pictures between the last two).
 *
 * Resources are loaded once and kept (meshes, textures, palettes, in GPU buffers that only grow). When
 * a buffer is full, the loader calls resources_reset and loads again what it needs. */
#ifndef PORT_RENDER_H
#define PORT_RENDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EL_RENDER_API_VERSION 6

/* ---- Resources: what the model loader makes ---------------------------------------------------- */

/* A vertex as the RSP holds it after a VTX command, before the per-draw part (matrix, lights, fog). */
typedef struct ElRdVertex {
    float x, y, z;      /* object space (the VTX's s16) */
    float s, t;         /* texel units: the VTX's S10.5 times the TEXTURE scale, as the RSP rounds it */
    uint8_t c[4];       /* colour RGBA, or with EL_RDV_LIGHT the normal (s8 x, y, z) and the alpha */
    uint32_t flags;     /* EL_RDV_*: the geometry mode at the VTX */
    uint16_t scale[2];  /* the TEXTURE scale (0.16), for EL_RDV_TEXGEN */
} ElRdVertex;           /* 32 bytes */

enum {
    EL_RDV_LIGHT = 1 << 0,      /* G_LIGHTING: c is a normal; the colour is lit per draw */
    EL_RDV_TEXGEN = 1 << 1,     /* G_TEXTURE_GEN: s, t from the normal and the LookAt */
    EL_RDV_TEXGEN_LIN = 1 << 2, /* G_TEXTURE_GEN_LINEAR */
    EL_RDV_FOG = 1 << 3,        /* G_FOG: the shade alpha is the fog factor */
    EL_RDV_SHADE = 1 << 4,      /* G_SHADE */
};

/* A texture tile as the RDP samples it: the decoded texels (EL_RDT_*) plus the tile's addressing. */
typedef struct ElRdTile {
    uint32_t texels;            /* offset of the texels (resources' texel buffer), 32-bit each */
    uint16_t width, height;     /* the texels decoded: the tile's masked or clamped area */
    uint8_t format;             /* EL_RDT_* */
    uint8_t cms, cmt;           /* G_TX_MIRROR 1 | G_TX_CLAMP 2 (clamp is forced when the mask is 0) */
    uint8_t masks, maskt, shifts, shiftt;
    uint8_t pad;
    uint16_t palette;           /* EL_RDT_CI: the palette (ElRenderApi.palette) */
    int16_t sl, tl, sh, th;     /* SETTILESIZE, 10.2 */
} ElRdTile;

enum {
    EL_RDT_NONE = 0,  /* no texture */
    EL_RDT_RGBA = 1,  /* texels are RGBA8 (R in the low byte) */
    EL_RDT_CI = 2,    /* texels are palette indices (0..255) into `palette` */
};

/* What a group of triangles is drawn with: the RDP and RSP state the loader saw at its triangles. */
typedef struct ElRdMaterial {
    uint32_t combine[2];        /* SETCOMBINE w0 (low 24 bits) and w1 */
    uint32_t othermode_h, othermode_l;
    uint32_t geometry;          /* the RSP geometry mode at the triangles (cull, zbuffer, shade) */
    uint8_t prim[4], env[4], blend[4], fog[4];
    uint32_t own;               /* EL_RDM_OWN_*: colours the list set itself (else the draw's) */
    uint8_t prim_lod_frac, k4, k5, tile_index; /* tile_index: the TEXTURE's render tile */
    uint32_t flags;             /* EL_RDM_* */
    ElRdTile tile[2];           /* TEXEL0 and TEXEL1 (the render tile and the next) */
} ElRdMaterial;

enum {
    EL_RDM_OWN_PRIM = 1 << 0,
    EL_RDM_OWN_ENV = 1 << 1,
    EL_RDM_OWN_BLEND = 1 << 2,
    EL_RDM_OWN_FOG = 1 << 3,
};

enum {
    EL_RDM_TEXTURE = 1 << 0,     /* the TEXTURE command turned texturing on */
    EL_RDM_TILE_SCROLL = 1 << 1, /* tile 0's size came from the entry state: the draw's tile offset moves it */
};

/* A run of triangles (3 vertices each) with one material. */
typedef struct ElRdGroup {
    uint32_t first, count;  /* vertices within the mesh */
    uint32_t material;      /* index into ElRdMeshDesc.materials */
} ElRdGroup;

typedef struct ElRdMeshDesc {
    const ElRdVertex *vertices;
    uint32_t vertex_count;
    const ElRdGroup *groups;
    uint32_t group_count;
    const ElRdMaterial *materials;
    uint32_t material_count;
    float bounds[6];        /* object-space box (min xyz, max xyz) over the triangles */
} ElRdMeshDesc;

/* ---- Per tick: the recording --------------------------------------------------------------------- */

/* Matrices are the N64's: row vectors (v' = v * M), m[row][col], already decoded from the fixed-point
 * Mtx the RSP would load (so they are exactly the RSP's numbers). */
typedef struct ElRdCamera {
    float projection[16];   /* the projection stack: view * perspective * shake (Camera_Update) */
    float view[16];         /* view alone (texgen, R6) */
    float perspective[16];
    float shake[2];         /* the shake translate in clip space (0 0: none) */
    int8_t lookat_x[3], lookat_y[3]; /* the LookAt (texgen) */
    uint16_t persp_norm;
    float near_plane, far_plane, fovy, aspect;
    float eye[3], at[3];    /* the inputs, for R6's pictures between */
} ElRdCamera;

/* Lights3 as the game fills it: ambient, then 3 directional lights (world-space directions) */
typedef struct ElRdLights {
    uint8_t ambient[3];
    uint8_t count;          /* directional lights (3) */
    uint8_t color[7][3];
    int8_t dir[7][3];
} ElRdLights;

/* What a draw item's matrix was made from (R6 rebuilds it from interpolated inputs): */
enum {
    EL_RDX_MATRIX = 0,   /* only the matrix: R6 snaps (or lerps) it */
    EL_RDX_POSITION = 1, /* guPosition(rot x, y, z in degrees, scale, pos) */
    EL_RDX_ROT_RAD = 2,  /* Math_MtxRotScaleTrans(rot x, y, z in radians, scale, pos) */
    EL_RDX_ALIGN_Y = 3,  /* Math_MtxFAlignYToVector(rot = the up vector x, y, z), pos as its translation,
                          * then Math_MtxFScaleAxes(scale, scale, scale) (shadows) */
    EL_RDX_ACTOR_NODE = 4, /* an actor model's node: Math_MtxRotScaleTrans(the actor's rot in radians, scale,
                              pos), times the node chain at the animation frame (ElRdDraw.inputs: EL_RDI_*) */
    EL_RDX_AIM_EYE = 5,  /* Math_MtxFAimAt(pos -> the camera's eye), then a roll of rot[2] radians about the view
                          * axis, then Math_MtxFScaleAxes(scale, more[0], 1) (effect sprites, stat-up markers):
                          * rebuilt with the interpolated eye, the item keeps facing the camera */
    EL_RDX_ALIGN_Y_AXES = 6, /* Math_MtxFAlignYToVector(rot = a direction), pos as the translation, then
                              * Math_MtxFScaleAxes(scale, more[0], more[1]) (streaks, terrain ring segments) */
    EL_RDX_EFFECT_NODE = 7, /* a model effect's node (effect.c Effect_DrawModelEffectGeometry): the root is
                             * inputs[EL_RDI_NODE] bit 18 (effect flag 4) ? AlignYToVector(rot = the up vector),
                             * pos, ScaleAxes(more[0..2] * 0.01) : RotScaleTrans(0, rot[1], 0, 0.01, pos),
                             * ScaleAxes(more[0..2]); times the node chain at frame inputs[EL_RDI_FRAME]
                             * (inputs as EL_RDX_ACTOR_NODE's; EL_RDI_ANIM_SET is the effect's AnimSet copy) */
};

typedef struct ElRdDraw {
    uint64_t id;            /* stable over ticks: (kind << 56) | object (a MapObj's address, a slot) */
    uint32_t mesh;          /* ElRenderApi.mesh_upload's id */
    uint32_t flags;         /* EL_RDD_* */
    float matrix[16];       /* model to world (N64 layout), decoded from the Mtx the RSP would load */
    uint32_t xform;         /* EL_RDX_* */
    float pos[3], rot[3], scale;
    float tile_offset[2];   /* EL_RDM_TILE_SCROLL tiles: added to SL/TL (10.2 units) */
    uint32_t inputs[4];     /* more of the matrix's inputs, by xform (R6); EL_RDX_ACTOR_NODE: EL_RDI_* */
    float more[4];          /* more float inputs, by xform (EL_RDX_AIM_EYE, _ALIGN_Y_AXES, _EFFECT_NODE) */
} ElRdDraw;

/* ElRdDraw.inputs of an EL_RDX_ACTOR_NODE draw */
enum {
    EL_RDI_ANIM_SET = 0, /* the AnimSet's N64 address (its animation: a new one is a snap) */
    EL_RDI_FRAME = 1,    /* Actor.frame (the TCB tracks' time) | Actor.ticks << 16 (dlSeq flipbooks) */
    EL_RDI_NODE = 2,     /* node index | AnimNode.flags << 8 (1 billboard) | Anim_DrawModel's flags << 16 */
    EL_RDI_DL = 3,       /* the node's display list this tick (a dlSeq swap is a snap) */
};

enum {
    EL_RDD_KIND_SHIFT = 56,
};

/* ElRdDraw.flags */
enum {
    EL_RDD_SCREEN = 1 << 0, /* the matrix goes straight to clip space (the camera is not applied): the fade
                             * quad's ortho x identity, the letterbox */
};

/* The kinds of draw (the high byte of ElRdDraw.id, and --native-kinds bits: 1 << kind) */
enum {
    EL_KIND_MAP = 0,        /* map objects: static, wave, spline, spinner, translucent, scrolling (R2) */
    EL_KIND_BILLBOARD = 1,  /* map billboards and animated-texture billboards (R3) */
    EL_KIND_BACKDROP = 2,   /* the sky cylinder (R3) */
    EL_KIND_ACTOR = 3,      /* player, NPCs, chests, enemies, bosses (R3) */
    EL_KIND_SHADOW = 4,     /* blob shadows (R3) */
    EL_KIND_EFFECT = 5,     /* sprites, streaks, rings, model effects, markers, numbers (R4) */
    EL_KIND_SCREEN = 6,     /* fade quad, letterbox (R4) */
    EL_KIND_HUD = 7,        /* HUD, compass, markers (R5) */
    EL_KIND_TEXT = 8,       /* text windows (R5) */
    EL_KIND_MENU = 9,       /* pause, item and save menus (R5) */
    EL_KIND_PICTURE = 10,   /* title, intro, ending, picture cues (R5) */
    EL_KIND_COUNT = 11,
};

typedef struct ElRdFrameInfo {
    uint8_t clear[3];       /* the picture's clear colour (RGB8) */
    uint8_t mode;           /* core_game_mode */
} ElRdFrameInfo;

/* ---- 2D (R5): texture and fill rectangles, and triangles under the 2D projection --------------------
 *
 * A 2D item is drawn in the recording's order with the 3D items (the game's frame-list order). Its
 * place is in the game's 320x240 screen units. Colours are the item's own (the RDP's colour registers
 * when the game queued it); the material (material_2d) holds the rest of the RDP state: combiner,
 * othermodes, the texture tiles. No depth.
 *
 * A rectangle is the RDP's: it covers the pixels whose top-left corner is in [x0, x1) x [y0, y1) (fill
 * rectangles in FILL mode are made half-open by the game side), and its texel at pixel (x, y) is
 * s = s0 + (x - x0) * dsdx, t = t0 + (y - y0) * dtdy, sampled by the material's tile as the RDP does.
 *
 * For R7 (widescreen) each item says where it belongs (EL_RD2D_ANCHOR_*) and gives its screen
 * rectangle; R6 matches items between ticks by id. */
typedef struct ElRd2D {
    uint64_t id;            /* stable over ticks: (kind << 56) | (element << 40) | the item's number in it */
    uint32_t mesh;          /* EL_RD2D_RECT: a rectangle; else a mesh (mesh_upload: 2D triangles) */
    uint32_t material;      /* a rectangle's material (material_2d) */
    uint32_t flags;         /* EL_RD2D_ANCHOR_* | EL_RD2D_* */
    float rect[4];          /* the screen rectangle x0, y0, x1, y1 (320x240 units); a mesh's extent */
    float st[4];            /* a rectangle's s0, t0 (texels) and ds/dx, dt/dy (texels per pixel) */
    float matrix[16];       /* a mesh's object-to-clip matrix (N64 layout): its model matrix times the 2D
                             * projection (the game's ortho times its translate) */
    float model[16];        /* a mesh's model matrix alone (R6, R7) */
    uint8_t env[4], prim[4], blend[4], fog[4]; /* the colour registers */
} ElRd2D;

enum { EL_RD2D_RECT = 0xFFFFFFFFu };

enum {
    EL_RD2D_ANCHOR_CENTRE = 0,  /* keeps its place in a 320-wide centre (menus, text, pictures) */
    EL_RD2D_ANCHOR_LEFT = 1,    /* moves with the left edge */
    EL_RD2D_ANCHOR_RIGHT = 2,   /* moves with the right edge */
    EL_RD2D_ANCHOR_STRETCH = 3, /* covers the whole width (fades, letterbox, full-screen fills) */
    EL_RD2D_ANCHOR_WORLD = 4,   /* placed from a 3D position (Camera_WorldToScreen: markers, numbers) */
    EL_RD2D_ANCHOR_MASK = 7,
    EL_RD2D_FILL = 1 << 3,      /* a fill rectangle (FILL mode or an untextured rectangle) */
};

/* ---- The function table ---------------------------------------------------------------------------- */

typedef struct ElRenderApi {
    uint32_t version;       /* EL_RENDER_API_VERSION */

    /* Resources. Each returns ~0u when its buffer is full: then resources_reset and load again. */
    uint32_t (*texture_upload)(const uint32_t *texels, uint32_t count); /* -> offset in the texel buffer */
    uint32_t (*mesh_upload)(const ElRdMeshDesc *mesh);                  /* -> mesh id */
    /* a palette's entries (raw 16-bit, RGBA5551 or IA16 by the material's TLUT mode) */
    void (*palette)(uint32_t id, const uint16_t *entries, uint32_t count);
    void (*resources_reset)(void);

    /* The tick */
    void (*frame_begin)(const ElRdFrameInfo *info);
    void (*camera)(const ElRdCamera *camera);
    void (*lights)(const ElRdLights *lights);
    void (*env_color)(const uint8_t rgba[4]);
    void (*prim_color)(const uint8_t rgba[4]);
    void (*fog_color)(const uint8_t rgba[4]);
    void (*fog_position)(int16_t multiplier, int16_t offset); /* gSPFogPosition's fm and fo */
    void (*draw)(const ElRdDraw *draw);
    void (*frame_end)(uint32_t frame);

    /* 2D (R5). material_2d: a material for 2D rectangles (its colours are the items'), loaded once
     * (~0u when full: then resources_reset). draw_2d: a rectangle or a 2D mesh, in order with draw. */
    uint32_t (*material_2d)(const ElRdMaterial *material);
    void (*draw_2d)(const ElRd2D *item);

    /* R6: where in its retrace the next frame_end comes (0..1, the game's virtual clock): the recording's
     * time, from which pictures between ticks are timed */
    void (*frame_phase)(float phase);

    /* R7b (modern options): the draw distance, 1 = the game's (the Graphics page's Draw distance; the fog and
     * the far plane go this many times as far, and nd_map.c draws the map objects of the grid cells that far
     * too, drawing only) */
    float (*draw_distance)(void);
} ElRenderApi;

#ifdef __cplusplus
}
#endif

#endif
