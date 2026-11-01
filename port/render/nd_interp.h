// Pictures between ticks (R6, docs/renderer/NATIVE_DRAW.md "Pictures between ticks").
//
// The game records one tick at a time (nd_renderer.h, Recording). A picture between two recordings a
// (older) and b (newer) at a time t (0 < t < 1) is a's recording with what moves smoothly carried
// toward b: the camera, and each item's matrix (matched in b by its stable id). Everything else is
// a's as it is (snaps): the meshes (flipbook frames, animation swaps), the colours (the flicker; a fade
// and an effect's colour going one way over three ticks are lerped), the
// draw order, items born in b (they appear at b's own picture) and items that died (they stay until
// b's picture). t = 0 is a's picture and t = 1 is b's, exactly: the interpolation uses the correction
// form M(t) = lerp(Ma, Mb, t) + R(t) - lerp(R(0), R(1), t), where R is the rebuild from the inputs
// (the game's own math, ported), so its endpoints cancel bit for bit whatever R's rounding.
//
// The work is split in two: plan() once per pair (matching, decompositions, the endpoints of R) and
// build() per picture (only the items that move).
#pragma once

#include "nd_renderer.h"

#include <cstdint>
#include <vector>

namespace render {

struct InterpStats {
    uint64_t plans = 0, pictures = 0;
    uint64_t items = 0, moving = 0, unmatched = 0, snapped = 0; // per plan, summed
    uint64_t cuts = 0;           // camera cuts (the whole picture snapped)
    double plan_us = 0, build_us = 0, plan_max_us = 0, build_max_us = 0;
};

class Interp {
public:
    // Plan the pictures between a and b. False when no picture between can be made (no camera in one,
    // a mode change, a camera cut): then the caller shows a as it is. `why` names the reason.
    // `before` (may be null): the tick before a, for the effects' colour ramps (a ramp lerps, a flicker snaps).
    bool plan(const Recording &a, const Recording &b, const Recording *before, const char **why);
    // The picture at t (0..1) of the planned pair. The returned recording stays valid until the next call.
    const Recording &build(float t);
    // The test of the ends (ELTALE_INTERP_ENDS): the matrices and the camera that differ, bit for bit, at
    // t = 0 from a's and at t = 1 from b's (the snaps aside, the picture at 1 is b's): 0 and 0
    void check_ends(int *at0, int *at1);

    InterpStats stats;

private:
    struct Decomp {
        float q[4];   // the rotation, a unit quaternion
        float s[9];   // the stretch (symmetric): L = S * Q, row vectors
        float t[3];
        bool ok;
    };
    struct Move {
        uint32_t item;      // index in a's items
        uint32_t b;         // b's item
        uint32_t xform, kind;
        bool local_snap;    // the local part (animation chain, roll) keeps a's (an animation switch)
        Decomp la, lb;      // the local parts (M * Root^-1) at a and b
        float e0[16], e1[16]; // R(0), R(1)
        float ma[16], mb[16];
    };
    struct Move2D {
        uint32_t item;      // index in a's items2d
        uint32_t b;
        bool rect, mesh, color;
        Decomp la, lb;      // a 2D mesh's model matrices
        float p[16];        // a's projection part: model_a^-1 * matrix_a
        float e0[16], e1[16];
    };
    struct Fade {
        uint32_t item, state; // a's item, the state appended for it
        uint8_t ea[4], eb[4];
    };

    // the root at input time t and camera time cam_t (between: a billboard's yaw from the camera)
    void root(const Move &m, const ElRdDraw &da, const ElRdDraw &db, float t, float cam_t, bool between, float *out) const;
    void eval(const Move &m, float t, float *out) const;
    void camera_at(float t, ElRdCamera &c) const;
    // the eye and the point looked at at t: lerped, or (orbit) round the point looked at
    void eye_at(float t, float *eye, float *at) const;

    const Recording *a = nullptr, *b = nullptr;
    Recording out;
    std::vector<Move> moves;
    std::vector<Move2D> moves2d;
    std::vector<Fade> fades;
    // the camera
    float va[16], vb[16], pa[16], pb[16], qa[16], qb[16]; // view, perspective, projection at a and b
    float cv0[16], cv1[16], cp0[16], cp1[16], cq0[16], cq1[16]; // the rebuilds' endpoints
    float eye_a[3], eye_b[3], at_a[3], at_b[3]; // eye: the view matrix's (with the height oscillation)
    float aim_a[3], aim_b[3]; // the eye the effects aim at (camera_state's)
    bool orbit = false;       // both ticks' views are the mouse camera's (EL_RDCAM_ORBIT)
};

// Matrix helpers (row vectors, m[row * 4 + col]), shared with the renderer's tests
void interp_mul(const float *a, const float *b, float *c);

} // namespace render
