// Pictures between ticks (nd_interp.h; docs/renderer/NATIVE_DRAW.md "Pictures between ticks").
//
// Each item's matrix is split into a root, rebuilt from the item's recorded inputs with the game's own
// math (ported below: Math_MtxFRotScaleTrans, guPosition, Math_MtxFAlignYToVector, Math_MtxFAimAt),
// and a local part L = M * Root^-1: an actor node's animation chain, a sprite's roll and scale, a
// shadow's scale. Between ticks the root's inputs are interpolated (positions linearly, angles the
// short way round, a billboard's yaw from the interpolated camera, an aimed sprite's eye from the
// interpolated eye) and the local part by its polar decomposition (rotation slerped, stretch and
// translation lerped). With R(t) = L(t) * Root(t), the picture's matrix is
//     M(t) = lerp(Ma, Mb, t) + R(t) - lerp(R(0), R(1), t)
// so t = 0 gives Ma and t = 1 gives Mb bit for bit, and between them R's path (an arc where the root
// turns, the slerped animation) with the small difference between the game's fixed-point matrices and
// the rebuild spread evenly.
#include "nd_interp.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace render {

namespace {

constexpr float PI = 3.14159265358979323846f;
// a jump between two ticks larger than this (world units) is a teleport or a cut: snap
constexpr float JUMP = 200.0f;
// the camera: a cut when the eye or the point looked at moves more than this in a tick, or the view
// turns more than CUT_TURN (cosine) or the field of view changes more than CUT_FOVY degrees
constexpr float CUT_MOVE = 40.0f;   // (walking moves the eye 1-5 a tick; a battle's camera cut 80)
constexpr float CUT_TURN = 0.9f;    // ~26 degrees
constexpr float CUT_ZOOM = 0.2f;    // the eye's distance from the point looked at changes by a fifth
constexpr float CUT_FOVY = 15.0f;
// 2D: a rectangle that moves more than this (pixels) in a tick snaps
constexpr float JUMP_2D = 24.0f;

double now_us() {
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void ident(float *m) {
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// the inverse of an affine matrix (v' = v * L + T): false if L is singular
bool affine_inverse(const float *m, float *out) {
    const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const double det = a * A + b * B + c * C;
    if (!(std::fabs(det) > 1e-20)) return false;
    const double r = 1.0 / det;
    double inv[9] = { A * r, -(b * i - c * h) * r, (b * f - c * e) * r,
                      B * r, (a * i - c * g) * r, -(a * f - c * d) * r,
                      C * r, -(a * h - b * g) * r, (a * e - b * d) * r };
    for (int k = 0; k < 3; k++) {
        out[k * 4 + 0] = float(inv[k * 3 + 0]);
        out[k * 4 + 1] = float(inv[k * 3 + 1]);
        out[k * 4 + 2] = float(inv[k * 3 + 2]);
        out[k * 4 + 3] = 0.0f;
    }
    for (int col = 0; col < 3; col++) {
        double s = 0;
        for (int k = 0; k < 3; k++) s += double(m[12 + k]) * inv[k * 3 + col];
        out[12 + col] = float(-s);
    }
    out[15] = 1.0f;
    return true;
}

float wrap_pi(float a) {
    a = std::fmod(a + PI, 2 * PI);
    if (a < 0) a += 2 * PI;
    return a - PI;
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }
float lerp_angle(float a, float b, float t, float turn) {
    float d = std::fmod(b - a, turn);
    if (d > turn / 2) d -= turn;
    if (d < -turn / 2) d += turn;
    return a + d * t;
}

// ---- the game's math (decomp src/game/math/math.c, libultra gu), in float as the game computes it ----

// Math_MtxFRotScaleTrans(rx, ry, rz in radians, scale, x, y, z)
void rot_scale_trans(float rx, float ry, float rz, float s, float x, float y, float z, float *m) {
    const float s1 = std::sin(rx), c1 = std::cos(rx), s2 = std::sin(ry), c2 = std::cos(ry), s3 = std::sin(rz),
                c3 = std::cos(rz);
    m[0] = ((c3 * c2) + ((s3 * s1) * s2)) * s;
    m[1] = (s3 * c1) * s;
    m[2] = ((-s2 * c3) + ((s3 * s1) * c2)) * s;
    m[3] = 0;
    m[4] = ((-s3 * c2) + ((c3 * s1) * s2)) * s;
    m[5] = (c3 * c1) * s;
    m[6] = ((-s3 * -s2) + ((c3 * s1) * c2)) * s;
    m[7] = 0;
    m[8] = (c1 * s2) * s;
    m[9] = -s1 * s;
    m[10] = (c1 * c2) * s;
    m[11] = 0;
    m[12] = x; m[13] = y; m[14] = z; m[15] = 1;
}

// guPositionF(r, p, h in degrees, scale, x, y, z)
void gu_position(float r, float p, float h, float s, float x, float y, float z, float *m) {
    const float dtor = 3.1415926f / 180.0f;
    r *= dtor; p *= dtor; h *= dtor;
    const float sr = std::sin(r), cr = std::cos(r), sp = std::sin(p), cp = std::cos(p), sh = std::sin(h), ch = std::cos(h);
    m[0] = (cp * ch) * s;
    m[1] = (cp * sh) * s;
    m[2] = (-sp) * s;
    m[3] = 0;
    m[4] = (sr * sp * ch - cr * sh) * s;
    m[5] = (sr * sp * sh + cr * ch) * s;
    m[6] = (sr * cp) * s;
    m[7] = 0;
    m[8] = (cr * sp * ch + sr * sh) * s;
    m[9] = (cr * sp * sh - sr * ch) * s;
    m[10] = (cr * cp) * s;
    m[11] = 0;
    m[12] = x; m[13] = y; m[14] = z; m[15] = 1;
}

// Math_MtxFAlignYToVector(vx, vy, vz), then x, y, z as the translation
void align_y(float vx, float vy, float vz, float x, float y, float z, float *m) {
    float s2, c2;
    const float d = std::sqrt(vx * vx + vz * vz);
    if (d != 0.0f) {
        s2 = vx / d;
        c2 = vz / d;
    } else {
        s2 = 0.0f;
        c2 = vy >= 0.0f ? 1.0f : -1.0f;
    }
    const float e = std::sqrt(d * d + vy * vy);
    const float s1 = e != 0.0f ? d / e : 0.0f, c1 = e != 0.0f ? vy / e : 1.0f;
    m[0] = c2; m[1] = 0; m[2] = -s2; m[3] = 0;
    m[4] = s1 * s2; m[5] = c1; m[6] = s1 * c2; m[7] = 0;
    m[8] = c1 * s2; m[9] = -s1; m[10] = c1 * c2; m[11] = 0;
    m[12] = x; m[13] = y; m[14] = z; m[15] = 1;
}

// Math_MtxFAimAt(from x y z, to x y z)
void aim_at(float x, float y, float z, float ex, float ey, float ez, float *m) {
    const float dx = ex - x, dz = ez - z;
    const float h = std::sqrt(dx * dx + dz * dz);
    float sy, cy, sp, cp;
    if (h == 0.0f) { sy = 0; cy = 1; } else { sy = dx / h; cy = dz / h; }
    const float dy = ey - y;
    const float l = std::sqrt(h * h + dy * dy);
    if (l == 0.0f) { sp = 0; cp = 1; } else { sp = -dy / l; cp = h / l; }
    m[0] = cy; m[1] = 0; m[2] = -sy; m[3] = 0;
    m[4] = sp * sy; m[5] = cp; m[6] = sp * cy; m[7] = 0;
    m[8] = cp * sy; m[9] = -sp; m[10] = cp * cy; m[11] = 0;
    m[12] = x; m[13] = y; m[14] = z; m[15] = 1;
}

// guLookAtReflectF's matrix (up 0 1 0), and its LookAt (texgen) directions
void look_at(const float *eye, const float *at, float *m, int8_t *lx, int8_t *ly) {
    float xl = at[0] - eye[0], yl = at[1] - eye[1], zl = at[2] - eye[2];
    float len = -1.0f / std::sqrt(xl * xl + yl * yl + zl * zl);
    xl *= len; yl *= len; zl *= len;
    float xr = 1.0f * zl - 0.0f * yl, yr = 0.0f * xl - 0.0f * zl, zr = 0.0f * yl - 1.0f * xl;
    len = 1.0f / std::sqrt(xr * xr + yr * yr + zr * zr);
    xr *= len; yr *= len; zr *= len;
    float xu = yl * zr - zl * yr, yu = zl * xr - xl * zr, zu = xl * yr - yl * xr;
    len = 1.0f / std::sqrt(xu * xu + yu * yu + zu * zu);
    xu *= len; yu *= len; zu *= len;
    auto frac8 = [](float v) { return int8_t(int(std::fmin(v * 128.0f, 127.0f)) & 0xFF); };
    if (lx) { lx[0] = frac8(xr); lx[1] = frac8(yr); lx[2] = frac8(zr); }
    if (ly) { ly[0] = frac8(xu); ly[1] = frac8(yu); ly[2] = frac8(zu); }
    m[0] = xr; m[4] = yr; m[8] = zr; m[12] = -(eye[0] * xr + eye[1] * yr + eye[2] * zr);
    m[1] = xu; m[5] = yu; m[9] = zu; m[13] = -(eye[0] * xu + eye[1] * yu + eye[2] * zu);
    m[2] = xl; m[6] = yl; m[10] = zl; m[14] = -(eye[0] * xl + eye[1] * yl + eye[2] * zl);
    m[3] = 0; m[7] = 0; m[11] = 0; m[15] = 1;
}

// guPerspectiveF(fovy degrees, aspect, near, far, scale 1)
void perspective(float fovy, float aspect, float n, float f, float *m) {
    std::memset(m, 0, 16 * sizeof(float));
    const float r = fovy * (3.1415926f / 180.0f);
    const float cot = std::cos(r / 2) / std::sin(r / 2);
    m[0] = cot / aspect;
    m[5] = cot;
    m[10] = (n + f) / (n - f);
    m[11] = -1;
    m[14] = (2 * n * f) / (n - f);
    m[15] = 0;
}

// the view matrix's eye (the camera's place: -T * R^T)
void view_eye(const float *v, float *eye) {
    for (int i = 0; i < 3; i++) eye[i] = -(v[12] * v[i * 4 + 0] + v[13] * v[i * 4 + 1] + v[14] * v[i * 4 + 2]);
}

// ---- decomposition ---------------------------------------------------------------------------------

void mat3_to_quat(const double *r, float *q) {
    // r: a rotation (rows); the standard conversion, used the same way both ways
    const double tr = r[0] + r[4] + r[8];
    double x, y, z, w;
    if (tr > 0) {
        double s = std::sqrt(tr + 1.0) * 2;
        w = 0.25 * s; x = (r[5] - r[7]) / s; y = (r[6] - r[2]) / s; z = (r[1] - r[3]) / s;
    } else if (r[0] > r[4] && r[0] > r[8]) {
        double s = std::sqrt(1.0 + r[0] - r[4] - r[8]) * 2;
        w = (r[5] - r[7]) / s; x = 0.25 * s; y = (r[3] + r[1]) / s; z = (r[6] + r[2]) / s;
    } else if (r[4] > r[8]) {
        double s = std::sqrt(1.0 + r[4] - r[0] - r[8]) * 2;
        w = (r[6] - r[2]) / s; x = (r[3] + r[1]) / s; y = 0.25 * s; z = (r[7] + r[5]) / s;
    } else {
        double s = std::sqrt(1.0 + r[8] - r[0] - r[4]) * 2;
        w = (r[1] - r[3]) / s; x = (r[6] + r[2]) / s; y = (r[7] + r[5]) / s; z = 0.25 * s;
    }
    const double l = std::sqrt(x * x + y * y + z * z + w * w);
    q[0] = float(x / l); q[1] = float(y / l); q[2] = float(z / l); q[3] = float(w / l);
}

void quat_to_mat3(const float *q, float *r) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    r[0] = 1 - 2 * (y * y + z * z); r[1] = 2 * (x * y + z * w);     r[2] = 2 * (x * z - y * w);
    r[3] = 2 * (x * y - z * w);     r[4] = 1 - 2 * (x * x + z * z); r[5] = 2 * (y * z + x * w);
    r[6] = 2 * (x * z + y * w);     r[7] = 2 * (y * z - x * w);     r[8] = 1 - 2 * (x * x + y * y);
}

// M's upper 3x3 = S * Q (Q a rotation, S symmetric), and the translation
bool decompose(const float *m, float *q, float *s, float *t) {
    double l[9], r[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) l[i * 3 + j] = r[i * 3 + j] = m[i * 4 + j];
    const double det = l[0] * (l[4] * l[8] - l[5] * l[7]) - l[1] * (l[3] * l[8] - l[5] * l[6]) + l[2] * (l[3] * l[7] - l[4] * l[6]);
    if (!(std::fabs(det) > 1e-12) || !std::isfinite(det)) return false;
    if (det < 0) for (double &v : r) v = -v; // a mirror: the rotation of -L, the stretch takes the sign
    // polar decomposition: Q = (Q + Q^-T) / 2 until it settles
    for (int it = 0; it < 30; it++) {
        const double a = r[0], b = r[1], c = r[2], d = r[3], e = r[4], f = r[5], g = r[6], h = r[7], i = r[8];
        const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
        const double dt = a * A + b * B + c * C;
        if (!(std::fabs(dt) > 1e-30)) return false;
        // inverse transpose = cofactors / det
        const double it_[9] = { A / dt, B / dt, C / dt,
                                -(b * i - c * h) / dt, (a * i - c * g) / dt, -(a * h - b * g) / dt,
                                (b * f - c * e) / dt, -(a * f - c * d) / dt, (a * e - b * d) / dt };
        double change = 0;
        for (int k = 0; k < 9; k++) {
            const double n = 0.5 * (r[k] + it_[k]);
            change += std::fabs(n - r[k]);
            r[k] = n;
        }
        if (change < 1e-12) break;
    }
    mat3_to_quat(r, q);
    float rq[9];
    quat_to_mat3(q, rq);
    // S = L * Q^T
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double v = 0;
            for (int k = 0; k < 3; k++) v += l[i * 3 + k] * rq[j * 3 + k];
            s[i * 3 + j] = float(v);
        }
    t[0] = m[12]; t[1] = m[13]; t[2] = m[14];
    return true;
}

void slerp(const float *a, const float *b_, float t, float *out) {
    float b[4] = { b_[0], b_[1], b_[2], b_[3] };
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0) { d = -d; for (float &v : b) v = -v; }
    float wa, wb;
    if (d > 0.9995f) {
        wa = 1 - t; wb = t;
    } else {
        const float th = std::acos(d), st = std::sin(th);
        wa = std::sin((1 - t) * th) / st;
        wb = std::sin(t * th) / st;
    }
    float l = 0;
    for (int k = 0; k < 4; k++) { out[k] = wa * a[k] + wb * b[k]; l += out[k] * out[k]; }
    l = 1.0f / std::sqrt(l);
    for (int k = 0; k < 4; k++) out[k] *= l;
}

// slerped rotation, lerped stretch and translation -> a matrix
template <typename D> void compose(const D &a, const D &b, float t, float *m) {
    float q[4], r[9], s[9];
    slerp(a.q, b.q, t, q);
    quat_to_mat3(q, r);
    for (int k = 0; k < 9; k++) s[k] = lerp(a.s[k], b.s[k], t);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) m[i * 4 + j] = s[i * 3 + 0] * r[0 * 3 + j] + s[i * 3 + 1] * r[1 * 3 + j] + s[i * 3 + 2] * r[2 * 3 + j];
        m[i * 4 + 3] = 0;
    }
    m[12] = lerp(a.t[0], b.t[0], t); m[13] = lerp(a.t[1], b.t[1], t); m[14] = lerp(a.t[2], b.t[2], t);
    m[15] = 1;
}

// out = lerp(ma, mb, t) + e - lerp(e0, e1, t), in that order (t = 0 and 1 give ma and mb exactly)
void correct(const float *ma, const float *mb, const float *e, const float *e0, const float *e1, float t, float *out) {
    for (int k = 0; k < 16; k++) {
        const float c = e[k] - (e0[k] * (1 - t) + e1[k] * t);
        out[k] = (ma[k] * (1 - t) + mb[k] * t) + c;
    }
}

bool same(const float *a, const float *b, int n) { return std::memcmp(a, b, size_t(n) * sizeof(float)) == 0; }

float dist3(const float *a, const float *b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

uint32_t kind_of(uint64_t id) { return uint32_t(id >> EL_RDD_KIND_SHIFT); }

// matches b's items to a's by id, the k-th of an id in a with the k-th in b
template <typename IdA, typename IdB> void match(size_t na, size_t nb, IdA id_a, IdB id_b, std::vector<uint32_t> &out) {
    std::unordered_map<uint64_t, uint32_t> first;
    std::vector<uint32_t> next(nb, ~0u);
    first.reserve(nb * 2);
    std::unordered_map<uint64_t, uint32_t> tail;
    tail.reserve(nb * 2);
    for (uint32_t i = 0; i < nb; i++) {
        const uint64_t id = id_b(i);
        auto it = tail.find(id);
        if (it == tail.end()) { first[id] = i; tail[id] = i; }
        else { next[it->second] = i; it->second = i; }
    }
    out.assign(na, ~0u);
    std::unordered_map<uint64_t, uint32_t> cursor;
    cursor.reserve(na * 2);
    for (uint32_t i = 0; i < na; i++) {
        const uint64_t id = id_a(i);
        auto c = cursor.find(id);
        uint32_t j;
        if (c == cursor.end()) {
            auto f = first.find(id);
            j = f == first.end() ? ~0u : f->second;
        } else {
            j = c->second == ~0u ? ~0u : next[c->second];
        }
        cursor[id] = j;
        out[i] = j;
    }
}

} // namespace

void interp_mul(const float *a, const float *b, float *c) {
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += double(a[i * 4 + k]) * b[k * 4 + j];
            r[i * 4 + j] = float(s);
        }
    std::memcpy(c, r, sizeof r);
}

// ---- the camera ------------------------------------------------------------------------------------

void Interp::camera_at(float t, ElRdCamera &c) const {
    // the view: rebuilt from the interpolated eye and point looked at; the perspective from the
    // interpolated field of view; the shake snaps (a's until b's own picture)
    float eye[3], at[3], v[16], p[16], q[16], s[16];
    for (int k = 0; k < 3; k++) { eye[k] = lerp(eye_a[k], eye_b[k], t); at[k] = lerp(at_a[k], at_b[k], t); }
    look_at(eye, at, v, c.lookat_x, c.lookat_y);
    perspective(lerp(a->camera.fovy, b->camera.fovy, t), a->camera.aspect, lerp(a->camera.near_plane, b->camera.near_plane, t),
                lerp(a->camera.far_plane, b->camera.far_plane, t), p);
    correct(va, vb, v, cv0, cv1, t, c.view);
    correct(pa, pb, p, cp0, cp1, t, c.perspective);
    interp_mul(v, p, q);
    ident(s);
    const bool to_b = t >= 1.0f;
    s[12] = to_b ? b->camera.shake[0] : a->camera.shake[0];
    s[13] = to_b ? b->camera.shake[1] : a->camera.shake[1];
    interp_mul(q, s, q);
    correct(qa, qb, q, cq0, cq1, t, c.projection);
    c.shake[0] = s[12];
    c.shake[1] = s[13];
    c.fovy = lerp(a->camera.fovy, b->camera.fovy, t);
    c.near_plane = lerp(a->camera.near_plane, b->camera.near_plane, t);
    c.far_plane = lerp(a->camera.far_plane, b->camera.far_plane, t);
    for (int k = 0; k < 3; k++) {
        c.eye[k] = lerp(aim_a[k], aim_b[k], t);
        c.at[k] = at[k];
    }
}

// ---- items ----------------------------------------------------------------------------------------

// The item's root at t, from its inputs (da's and db's)
void Interp::root(const Move &m, const ElRdDraw &da, const ElRdDraw &db, float t, float cam_t, bool between,
                  float *out) const {
    float pos[3];
    for (int k = 0; k < 3; k++) pos[k] = lerp(da.pos[k], db.pos[k], t);
    const float scale = lerp(da.scale, db.scale, t);
    switch (m.xform) {
    case EL_RDX_POSITION:
        gu_position(lerp_angle(da.rot[0], db.rot[0], t, 360), lerp_angle(da.rot[1], db.rot[1], t, 360),
                    lerp_angle(da.rot[2], db.rot[2], t, 360), scale, pos[0], pos[1], pos[2], out);
        return;
    case EL_RDX_ROT_RAD:
    case EL_RDX_ACTOR_NODE: {
        float yaw = lerp_angle(da.rot[1], db.rot[1], t, 2 * PI);
        if (m.kind == EL_KIND_BILLBOARD && between) {
            // turned with the interpolated camera (the game gives it the camera's yaw)
            float e[3], at[3];
            for (int k = 0; k < 3; k++) { e[k] = lerp(eye_a[k], eye_b[k], cam_t); at[k] = lerp(at_a[k], at_b[k], cam_t); }
            const float y0 = std::atan2(eye_a[0] - at_a[0], eye_a[2] - at_a[2]), y1 = std::atan2(e[0] - at[0], e[2] - at[2]);
            yaw = da.rot[1] + wrap_pi(y1 - y0);
        }
        rot_scale_trans(lerp_angle(da.rot[0], db.rot[0], t, 2 * PI), yaw, lerp_angle(da.rot[2], db.rot[2], t, 2 * PI), scale,
                        pos[0], pos[1], pos[2], out);
        return;
    }
    case EL_RDX_ALIGN_Y:
    case EL_RDX_ALIGN_Y_AXES:
        align_y(lerp(da.rot[0], db.rot[0], t), lerp(da.rot[1], db.rot[1], t), lerp(da.rot[2], db.rot[2], t), pos[0], pos[1],
                pos[2], out);
        return;
    case EL_RDX_AIM_EYE: {
        float e[3];
        for (int k = 0; k < 3; k++) e[k] = lerp(aim_a[k], aim_b[k], cam_t);
        aim_at(pos[0], pos[1], pos[2], e[0], e[1], e[2], out);
        return;
    }
    case EL_RDX_EFFECT_NODE:
        if (da.inputs[EL_RDI_NODE] & (4u << 16))
            align_y(lerp(da.rot[0], db.rot[0], t), lerp(da.rot[1], db.rot[1], t), lerp(da.rot[2], db.rot[2], t), pos[0], pos[1],
                    pos[2], out);
        else
            rot_scale_trans(0, lerp_angle(da.rot[1], db.rot[1], t, 2 * PI), 0, 1, pos[0], pos[1], pos[2], out);
        return;
    default:
        ident(out);
        return;
    }
}

void Interp::eval(const Move &m, float t, float *out) const {
    const ElRdDraw &da = a->items[m.item].draw, &db = b->items[m.b].draw;
    float r[16], l[16];
    root(m, da, db, t, t, true, r);
    compose(m.la, m.local_snap ? m.la : m.lb, t, l);
    interp_mul(l, r, out);
}

bool Interp::plan(const Recording &ra, const Recording &rb, const Recording *rbefore, const char **why) {
    const double t0 = now_us();
    a = &ra;
    b = &rb;
    moves.clear();
    moves2d.clear();
    fades.clear();
    *why = nullptr;
    if (ra.info.mode != rb.info.mode) *why = "mode";
    else if (ra.has_camera != rb.has_camera) *why = "camera";
    if (*why) return false;

    if (ra.has_camera) {
        const ElRdCamera &ca = ra.camera, &cb = rb.camera;
        std::memcpy(va, ca.view, sizeof va); std::memcpy(vb, cb.view, sizeof vb);
        std::memcpy(pa, ca.perspective, sizeof pa); std::memcpy(pb, cb.perspective, sizeof pb);
        std::memcpy(qa, ca.projection, sizeof qa); std::memcpy(qb, cb.projection, sizeof qb);
        view_eye(va, eye_a);
        view_eye(vb, eye_b);
        std::memcpy(at_a, ca.at, sizeof at_a); std::memcpy(at_b, cb.at, sizeof at_b);
        std::memcpy(aim_a, ca.eye, sizeof aim_a); std::memcpy(aim_b, cb.eye, sizeof aim_b);
        // a cut: a jump, a sharp turn, a new field of view
        float da[3], db[3];
        for (int k = 0; k < 3; k++) { da[k] = at_a[k] - eye_a[k]; db[k] = at_b[k] - eye_b[k]; }
        const float la = std::sqrt(da[0] * da[0] + da[1] * da[1] + da[2] * da[2]);
        const float lb = std::sqrt(db[0] * db[0] + db[1] * db[1] + db[2] * db[2]);
        const float cosang = la > 0 && lb > 0 ? (da[0] * db[0] + da[1] * db[1] + da[2] * db[2]) / (la * lb) : 1.0f;
        static const bool log = std::getenv("ELTALE_INTERP_LOG") != nullptr;
        if (log)
            std::printf("P1_INTERP_CAMERA frame=%u eye=%.1f at=%.1f cos=%.4f fovy=%.2f dist=%.1f/%.1f\n", ra.frame,
                        dist3(eye_a, eye_b), dist3(at_a, at_b), cosang, cb.fovy - ca.fovy, la, lb);
        if (dist3(eye_a, eye_b) > CUT_MOVE || dist3(at_a, at_b) > CUT_MOVE || cosang < CUT_TURN ||
            std::fabs(ca.fovy - cb.fovy) > CUT_FOVY || !(la > 0) || !(lb > 0) || std::fabs(la - lb) > CUT_ZOOM * std::fmax(la, lb)) {
            stats.cuts++;
            *why = "cut";
            return false;
        }
        // the rebuilds' endpoints (the same code as between, so they cancel exactly)
        for (int e = 0; e < 2; e++) {
            const float t = float(e);
            float eye[3], at[3], v[16], p[16], q[16], s[16];
            for (int k = 0; k < 3; k++) { eye[k] = lerp(eye_a[k], eye_b[k], t); at[k] = lerp(at_a[k], at_b[k], t); }
            look_at(eye, at, v, nullptr, nullptr);
            perspective(lerp(ca.fovy, cb.fovy, t), ca.aspect, lerp(ca.near_plane, cb.near_plane, t),
                        lerp(ca.far_plane, cb.far_plane, t), p);
            interp_mul(v, p, q);
            ident(s);
            s[12] = e ? cb.shake[0] : ca.shake[0];
            s[13] = e ? cb.shake[1] : ca.shake[1];
            interp_mul(q, s, q);
            std::memcpy(e ? cv1 : cv0, v, sizeof v);
            std::memcpy(e ? cp1 : cp0, p, sizeof p);
            std::memcpy(e ? cq1 : cq0, q, sizeof q);
        }
    }

    out = ra; // the picture's base: a's recording (what snaps is a's)

    // 3D items
    std::vector<uint32_t> m3, m2;
    match(ra.items.size(), rb.items.size(),
          [&](uint32_t i) { return ra.items[i].item2d >= 0 ? ~0ull : ra.items[i].draw.id; },
          [&](uint32_t i) { return rb.items[i].item2d >= 0 ? ~1ull : rb.items[i].draw.id; }, m3);
    // the tick before a (an effect's colour ramp): a's items in it
    std::vector<uint32_t> mb4;
    if (rbefore && rbefore->info.mode == ra.info.mode)
        match(ra.items.size(), rbefore->items.size(),
              [&](uint32_t i) { return ra.items[i].item2d >= 0 ? ~0ull : ra.items[i].draw.id; },
              [&](uint32_t i) { return rbefore->items[i].item2d >= 0 ? ~1ull : rbefore->items[i].draw.id; }, mb4);
    else
        rbefore = nullptr;
    for (uint32_t i = 0; i < ra.items.size(); i++) {
        const Recording::Item &ia = ra.items[i];
        if (ia.item2d >= 0) continue;
        stats.items++;
        const uint32_t j = m3[i];
        if (j == ~0u) { stats.unmatched++; continue; }
        const Recording::Item &ib = rb.items[j];
        const ElRdDraw &da = ia.draw, &db = ib.draw;
        const uint32_t kind = kind_of(da.id);
        // colours: the fade quad's follow its ramp; an effect's follow a ramp too (its env colour going one
        // way over the last three ticks: a sprite fading in or out), but a flicker (going back and forth:
        // the odd and even ticks' colours) snaps, as the owner wants it
        {
            const uint8_t *ea = ra.states[ia.state].env, *eb = rb.states[ib.state].env;
            bool ramp = false;
            if (std::memcmp(ea, eb, 4) != 0) {
                if (kind == EL_KIND_SCREEN && ((da.id >> 48) & 0xFF) == 0) {
                    ramp = true;
                } else if (kind == EL_KIND_EFFECT && rbefore && mb4[i] != ~0u) {
                    const uint8_t *e0 = rbefore->states[rbefore->items[mb4[i]].state].env;
                    ramp = true;
                    for (int k = 0; k < 4 && ramp; k++) {
                        const int d0 = int(ea[k]) - int(e0[k]), d1 = int(eb[k]) - int(ea[k]);
                        if (d1 != 0 && (d0 == 0 || (d0 > 0) != (d1 > 0) || std::abs(d1) > 96)) ramp = false;
                    }
                }
            }
            if (ramp) {
                Fade f;
                f.item = i;
                f.state = uint32_t(out.states.size());
                std::memcpy(f.ea, ea, 4);
                std::memcpy(f.eb, eb, 4);
                out.states.push_back(ra.states[ia.state]);
                out.items[i].state = f.state;
                fades.push_back(f);
            }
        }
        if (da.flags & EL_RDD_SCREEN) continue;
        if (same(da.matrix, db.matrix, 16)) continue; // still
        Move m;
        m.item = i;
        m.b = j;
        m.xform = da.xform;
        m.kind = kind;
        std::memcpy(m.ma, da.matrix, sizeof m.ma);
        std::memcpy(m.mb, db.matrix, sizeof m.mb);
        // a teleport (a big jump of the root or of the matrix): snap
        float ta[3] = { da.matrix[12], da.matrix[13], da.matrix[14] }, tb[3] = { db.matrix[12], db.matrix[13], db.matrix[14] };
        if (dist3(da.pos, db.pos) > JUMP || dist3(ta, tb) > JUMP || da.xform != db.xform) { stats.snapped++; continue; }
        // the local parts
        float ra_[16], rb_[16], inv[16], la[16], lb[16];
        root(m, da, da, 0.0f, 0.0f, false, ra_);
        root(m, db, db, 0.0f, 1.0f, false, rb_);
        if (!affine_inverse(ra_, inv)) { stats.snapped++; continue; }
        interp_mul(da.matrix, inv, la);
        if (!affine_inverse(rb_, inv)) { stats.snapped++; continue; }
        interp_mul(db.matrix, inv, lb);
        m.la.ok = decompose(la, m.la.q, m.la.s, m.la.t);
        m.lb.ok = decompose(lb, m.lb.q, m.lb.s, m.lb.t);
        if (!m.la.ok || !m.lb.ok) { stats.snapped++; continue; }
        // an animation switch (a new AnimSet): the chain keeps a's pose, the root still moves
        m.local_snap = (da.xform == EL_RDX_ACTOR_NODE || da.xform == EL_RDX_EFFECT_NODE) &&
                       da.inputs[EL_RDI_ANIM_SET] != db.inputs[EL_RDI_ANIM_SET];
        eval(m, 0.0f, m.e0);
        eval(m, 1.0f, m.e1);
        moves.push_back(m);
    }
    stats.moving += moves.size();

    // 2D items
    match(ra.items2d.size(), rb.items2d.size(), [&](uint32_t i) { return ra.items2d[i].id; },
          [&](uint32_t i) { return rb.items2d[i].id; }, m2);
    for (uint32_t i = 0; i < ra.items2d.size(); i++) {
        const uint32_t j = m2[i];
        if (j == ~0u) continue;
        const ElRd2D &da = ra.items2d[i], &db = rb.items2d[j];
        const uint32_t kind = kind_of(da.id);
        if (kind == EL_KIND_TEXT || kind == EL_KIND_MENU) continue; // text windows and menus snap
        if (da.mesh != db.mesh || da.material != db.material) continue;
        Move2D m = {};
        m.item = i;
        m.b = j;
        if (da.mesh == EL_RD2D_RECT) {
            float d = 0;
            for (int k = 0; k < 4; k++) d = std::fmax(d, std::fabs(da.rect[k] - db.rect[k]));
            m.rect = d > 0 && d <= JUMP_2D;
        } else if (!same(da.model, db.model, 16)) {
            float inv[16];
            if (affine_inverse(da.model, inv) && decompose(da.model, m.la.q, m.la.s, m.la.t) &&
                decompose(db.model, m.lb.q, m.lb.s, m.lb.t) &&
                std::fabs(da.model[12] - db.model[12]) + std::fabs(da.model[13] - db.model[13]) <= JUMP_2D) {
                interp_mul(inv, da.matrix, m.p);
                float l[16];
                compose(m.la, m.lb, 0.0f, l);
                interp_mul(l, m.p, m.e0);
                compose(m.la, m.lb, 1.0f, l);
                interp_mul(l, m.p, m.e1);
                m.mesh = true;
            }
        }
        // full-screen fills (a picture's fade): their colours follow the ramp
        m.color = (da.flags & EL_RD2D_FILL) && (da.flags & EL_RD2D_ANCHOR_MASK) == EL_RD2D_ANCHOR_STRETCH &&
                  (std::memcmp(da.env, db.env, 4) || std::memcmp(da.prim, db.prim, 4) || std::memcmp(da.blend, db.blend, 4));
        if (m.rect || m.mesh || m.color) moves2d.push_back(m);
    }

    stats.plans++;
    const double us = now_us() - t0;
    stats.plan_us += us;
    if (us > stats.plan_max_us) stats.plan_max_us = us;
    return true;
}

const Recording &Interp::build(float t) {
    const double t0 = now_us();
    if (a->has_camera) {
        camera_at(t, out.camera);
    }
    for (const Move &m : moves) {
        float e[16];
        eval(m, t, e);
        correct(m.ma, m.mb, e, m.e0, m.e1, t, out.items[m.item].draw.matrix);
    }
    for (const Fade &f : fades) {
        uint8_t *env = out.states[f.state].env;
        for (int k = 0; k < 4; k++) env[k] = uint8_t(std::lround(lerp(float(f.ea[k]), float(f.eb[k]), t)));
    }
    for (const Move2D &m : moves2d) {
        const ElRd2D &da = a->items2d[m.item], &db = b->items2d[m.b];
        ElRd2D &o = out.items2d[m.item];
        if (m.rect)
            for (int k = 0; k < 4; k++) o.rect[k] = lerp(da.rect[k], db.rect[k], t);
        if (m.mesh) {
            float l[16], e[16];
            compose(m.la, m.lb, t, l);
            interp_mul(l, m.p, e);
            correct(da.matrix, db.matrix, e, m.e0, m.e1, t, o.matrix);
            std::memcpy(o.model, l, sizeof o.model); // (the model alone: R7's)
        }
        if (m.color)
            for (int k = 0; k < 4; k++) {
                o.env[k] = uint8_t(std::lround(lerp(float(da.env[k]), float(db.env[k]), t)));
                o.prim[k] = uint8_t(std::lround(lerp(float(da.prim[k]), float(db.prim[k]), t)));
                o.blend[k] = uint8_t(std::lround(lerp(float(da.blend[k]), float(db.blend[k]), t)));
            }
    }
    stats.pictures++;
    const double us = now_us() - t0;
    stats.build_us += us;
    if (us > stats.build_max_us) stats.build_max_us = us;
    return out;
}

void Interp::check_ends(int *at0, int *at1) {
    *at0 = *at1 = 0;
    const Recording &p0 = build(0.0f);
    if (a->has_camera && (!same(p0.camera.projection, a->camera.projection, 16) || !same(p0.camera.view, a->camera.view, 16)))
        (*at0)++;
    for (size_t i = 0; i < p0.items.size(); i++)
        if (p0.items[i].item2d < 0 && !same(p0.items[i].draw.matrix, a->items[i].draw.matrix, 16)) (*at0)++;
    for (size_t i = 0; i < p0.items2d.size(); i++)
        if (!same(p0.items2d[i].rect, a->items2d[i].rect, 4) || !same(p0.items2d[i].matrix, a->items2d[i].matrix, 16)) (*at0)++;
    const Recording &p1 = build(1.0f);
    if (a->has_camera && (!same(p1.camera.projection, b->camera.projection, 16) || !same(p1.camera.view, b->camera.view, 16)))
        (*at1)++;
    for (const Move &m : moves)
        if (!same(p1.items[m.item].draw.matrix, b->items[m.b].draw.matrix, 16)) (*at1)++;
    for (const Move2D &m : moves2d) {
        const ElRd2D &o = p1.items2d[m.item], &db = b->items2d[m.b];
        if ((m.rect && !same(o.rect, db.rect, 4)) || (m.mesh && !same(o.matrix, db.matrix, 16))) (*at1)++;
    }
}

} // namespace render
