// The area map (docs/AUTOMAP.md): the plan and the player the game's feed sends (port/automap.h,
// game/native_draw/nd_automap.c), drawn over the game with ImGui's draw lists: the full map (the map key, M by default:
// wheel zoom, drag to pan, right click to fit again; Esc or the key closes it) and the minimap.
//
// The look follows the King's Field maps (KF1, KF3, KF4): a dark backdrop, the floor in warm greys shaded by height
// (lower is darker) and by its slope (lit from the upper left), light wall lines, darker lines where the floor ends
// without a wall (a ledge, a drop, a step between levels), and the player as a light dart. Every size is a fraction of
// the game picture's height, so it looks the same at any window size, in 4:3 and 16:9.
//
// The arrow follows the pictures between ticks: the renderer says which two recordings (by frame number) the last
// picture was made from and how far between (NativeDraw::shown_frames), and the player samples carry the same numbers.
#include "host.h"
#include "nd_renderer.h"
#include "port/automap.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace host {
namespace automap {
namespace {

// ---- the palette (KF1/KF3/KF4's) ------------------------------------------------------------------
constexpr ImU32 COL_TITLE = IM_COL32(240, 220, 170, 255);
constexpr ImU32 COL_HELP = IM_COL32(170, 160, 140, 255);
constexpr ImU32 COL_WALL = IM_COL32(215, 200, 160, 255);
constexpr ImU32 COL_EDGE = IM_COL32(150, 132, 100, 255);
constexpr ImU32 COL_ARROW = IM_COL32(255, 250, 220, 255);
constexpr ImU32 COL_OUTLINE = IM_COL32(0, 0, 0, 255);
constexpr ImU32 COL_NORTH = IM_COL32(230, 60, 50, 255);
constexpr float FLOOR_R = 92, FLOOR_G = 83, FLOOR_B = 66;

// Screen x = -world x: the game's world is right-handed with y up, so looking north (+z) from above, +x is on the
// left (the truth view, nd_automap.c, shows the same).
constexpr float MX = -1.0f;

// ---- what the game sent --------------------------------------------------------------------------

struct Edge {
    float x0, z0, x1, z1;
};

struct Plan {
    uint32_t serial = 0;
    int map = -1, sub = -1;
    uint32_t flags = 0;
    float bounds[4] = {};
    float y_min = 0, y_max = 0;
    std::vector<ElAmTri> tris;
    std::vector<ElAmWall> walls;
    std::vector<ElAmExit> exits;
    std::vector<ElAmMarker> markers;
    // made from the above when it comes
    std::vector<uint32_t> order; // the triangles, lowest first (a higher floor is drawn over a lower one)
    std::vector<float> slope;    // per triangle: its light (1 flat)
    std::vector<Edge> edges;     // floor edges with no floor on the other side at that height (reachable floor's)
    std::vector<ElAmTri> water;
    std::vector<Edge> wet;   // water as rectangles (x0, z0, x1, z1), where it is seen from above (reach())
    // reachable floor (reach()): what the player can walk to from where they are (or, before that is known, from
    // the exits), walls in the way; the rest is drawn as out of reach (the slopes and fields beyond the walls)
    std::vector<uint8_t> reach;
    bool reach_done = false;
    float reach_box[4] = {}; // the reachable floor's bounds (the full map fits it)
    bool valid() const { return serial != 0; }
};
Plan plan;

// the player, by the tick's frame number (the native renderer's recordings carry the same)
struct Sample {
    uint32_t frame = 0, area = 0, flags = 0;
    float x = 0, y = 0, z = 0, rot = 0;
};
constexpr size_t SAMPLES = 64;
Sample samples[SAMPLES];
size_t sample_next;
Sample newest;
bool have_sample;

const Sample *find_sample(uint32_t frame) {
    for (size_t i = 0; i < SAMPLES; i++)
        if (samples[i].area && samples[i].frame == frame) return &samples[i];
    return nullptr;
}

// The floor's edges: a triangle side no other triangle shares (at the same height) is where the floor ends; with a
// wall there the wall line covers it, without one it is a ledge or a drop.
void make_edges(Plan &p) {
    struct Key {
        int64_t a, b;
        bool operator==(const Key &o) const { return a == o.a && b == o.b; }
    };
    struct Hash {
        size_t operator()(const Key &k) const { return size_t(k.a * 0x9E3779B97F4A7C15ull ^ (k.b + 0x632BE59BD9B4E019ull)); }
    };
    auto point = [](const float *v) {
        const int64_t x = int64_t(std::lround(v[0] * 2)), y = int64_t(std::lround(v[1] / 2)), z = int64_t(std::lround(v[2] * 2));
        return (x & 0x1FFFFF) << 42 | (y & 0x1FFFFF) << 21 | (z & 0x1FFFFF);
    };
    std::unordered_map<Key, int, Hash> count;
    count.reserve(p.tris.size() * 3);
    for (const ElAmTri &t : p.tris)
        for (int j = 0; j < 3; j++) {
            int64_t a = point(t.v[j]), b = point(t.v[(j + 1) % 3]);
            if (a > b) std::swap(a, b);
            count[{ a, b }]++;
        }
    p.edges.clear();
    for (size_t i = 0; i < p.tris.size(); i++)
        for (int j = 0; j < 3; j++) {
            if (i < p.reach.size() && !p.reach[i]) break;
            const ElAmTri &t = p.tris[i];
            const float *u = t.v[j], *v = t.v[(j + 1) % 3];
            int64_t a = point(u), b = point(v);
            if (a > b) std::swap(a, b);
            if (count[{ a, b }] == 1) p.edges.push_back({ u[0], u[2], v[0], v[2] });
        }
}

// Reachable floor. The game puts the player on whatever floor is under them at any height (no step limit:
// Player_MoveAndCollide takes the floor query's height), so only walls keep them in: the floor is rasterised into a
// grid (cells of 4 units or more, at most ~700 across), the wall segments into blocked cells (densely sampled: an
// 8-connected line no 4-connected fill crosses), and a fill from the seeds marks the cells walked to. A triangle with
// a cell reached is reachable.
void reach(Plan &p, const std::vector<std::pair<float, float>> &seeds) {
    const size_t n = p.tris.size();
    p.reach.assign(n, 0);
    p.reach_done = true;
    const float x0 = p.bounds[0], z0 = p.bounds[1];
    const float w = std::max(p.bounds[2] - x0, 1.0f), h = std::max(p.bounds[3] - z0, 1.0f);
    // (cells of 3 units, more on the biggest maps: at most ~4 million; a dungeon's corridor is 20-30 units wide, and
    // a wall marks the cells it passes through)
    const float cs = std::max(3.0f, std::sqrt(w * h / 4.0e6f));
    const int nx = int(w / cs) + 2, nz = int(h / cs) + 2;
    std::vector<int32_t> tri_of(size_t(nx) * size_t(nz), -1);
    std::vector<uint8_t> blocked(size_t(nx) * size_t(nz), 0), seen(size_t(nx) * size_t(nz), 0);
    auto cell = [&](float x, float z, int *ix, int *iz) {
        *ix = int(std::floor((x - x0) / cs));
        *iz = int(std::floor((z - z0) / cs));
        return *ix >= 0 && *iz >= 0 && *ix < nx && *iz < nz;
    };
    for (size_t i = 0; i < n; i++) {
        const ElAmTri &t = p.tris[i];
        const float ax = t.v[0][0], az = t.v[0][2], bx = t.v[1][0], bz = t.v[1][2], cx = t.v[2][0], cz = t.v[2][2];
        const float area = (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
        if (area == 0) continue;
        int i0, k0, i1, k1;
        cell(std::min({ ax, bx, cx }), std::min({ az, bz, cz }), &i0, &k0);
        cell(std::max({ ax, bx, cx }), std::max({ az, bz, cz }), &i1, &k1);
        i0 = std::max(i0, 0); k0 = std::max(k0, 0); i1 = std::min(i1, nx - 1); k1 = std::min(k1, nz - 1);
        for (int kz = k0; kz <= k1; kz++)
            for (int ix = i0; ix <= i1; ix++) {
                const float px = x0 + (float(ix) + 0.5f) * cs, pz = z0 + (float(kz) + 0.5f) * cs;
                const float w0 = ((bx - px) * (cz - pz) - (bz - pz) * (cx - px)) / area;
                const float w1 = ((cx - px) * (az - pz) - (cz - pz) * (ax - px)) / area;
                if (w0 < 0 || w1 < 0 || w0 + w1 > 1) continue;
                int32_t &c = tri_of[size_t(kz) * size_t(nx) + size_t(ix)];
                if (c < 0) c = int32_t(i);
            }
    }
    for (const ElAmWall &wl : p.walls) {
        const float dx = wl.x1 - wl.x0, dz = wl.z1 - wl.z0, len = std::sqrt(dx * dx + dz * dz);
        const int steps = std::max(1, int(len / (cs * 0.25f)));
        for (int k = 0; k <= steps; k++) {
            int ix, iz;
            if (cell(wl.x0 + dx * float(k) / float(steps), wl.z0 + dz * float(k) / float(steps), &ix, &iz))
                blocked[size_t(iz) * size_t(nx) + size_t(ix)] = 1;
        }
    }
    std::vector<int> queue;
    for (const auto &sd : seeds) {
        int ix, iz;
        if (!cell(sd.first, sd.second, &ix, &iz)) continue;
        // (the nearest open floor cell within 3)
        int best = -1, best_d = 1 << 30;
        for (int dz = -3; dz <= 3; dz++)
            for (int dx = -3; dx <= 3; dx++) {
                const int x = ix + dx, z = iz + dz;
                if (x < 0 || z < 0 || x >= nx || z >= nz) continue;
                const int c = z * nx + x;
                if (tri_of[size_t(c)] < 0 || blocked[size_t(c)]) continue;
                if (dx * dx + dz * dz < best_d) best_d = dx * dx + dz * dz, best = c;
            }
        if (best >= 0 && !seen[size_t(best)]) seen[size_t(best)] = 1, queue.push_back(best);
    }
    for (size_t q = 0; q < queue.size(); q++) {
        const int c = queue[q], x = c % nx, z = c / nx;
        const int nb[4][2] = { { x - 1, z }, { x + 1, z }, { x, z - 1 }, { x, z + 1 } };
        for (const auto &v : nb) {
            if (v[0] < 0 || v[1] < 0 || v[0] >= nx || v[1] >= nz) continue;
            const int d = v[1] * nx + v[0];
            if (seen[size_t(d)] || blocked[size_t(d)] || tri_of[size_t(d)] < 0) continue;
            seen[size_t(d)] = 1;
            queue.push_back(d);
        }
    }
    if (queue.empty()) { // (nothing to start from: all of it)
        std::fill(p.reach.begin(), p.reach.end(), 1);
    } else {
        for (int c : queue) p.reach[size_t(tri_of[size_t(c)])] = 1;
        // a triangle too small or thin for a cell's centre, or under a wall: its centre's cell and the four beside it
        for (size_t i = 0; i < n; i++) {
            if (p.reach[i]) continue;
            const ElAmTri &t = p.tris[i];
            int ix, iz;
            if (!cell((t.v[0][0] + t.v[1][0] + t.v[2][0]) / 3, (t.v[0][2] + t.v[1][2] + t.v[2][2]) / 3, &ix, &iz)) continue;
            const int nb[5][2] = { { ix, iz }, { ix - 1, iz }, { ix + 1, iz }, { ix, iz - 1 }, { ix, iz + 1 } };
            for (const auto &v : nb)
                if (v[0] >= 0 && v[1] >= 0 && v[0] < nx && v[1] < nz && seen[size_t(v[1] * nx + v[0])]) { p.reach[i] = 1; break; }
        }
    }
    float b[4] = { 1e30f, 1e30f, -1e30f, -1e30f };
    for (size_t i = 0; i < n; i++)
        if (p.reach[i])
            for (int j = 0; j < 3; j++) {
                b[0] = std::min(b[0], p.tris[i].v[j][0]); b[2] = std::max(b[2], p.tris[i].v[j][0]);
                b[1] = std::min(b[1], p.tris[i].v[j][2]); b[3] = std::max(b[3], p.tris[i].v[j][2]);
            }
    if (b[0] > b[2]) std::memcpy(b, p.bounds, sizeof b);
    std::memcpy(p.reach_box, b, sizeof b);

    // Water where it is seen from above: the water's surface over a cell, and either no floor there that is reached
    // from the grid's edge without crossing floor (the open sea and lakes; not a building's footprint inside the
    // floor, where the sea mesh runs on underneath) or a floor below the surface (a pond's bed). Drawn as runs of
    // cells over the floor.
    p.wet.clear();
    if (!p.water.empty()) {
        std::vector<float> wy(size_t(nx) * size_t(nz), -1e30f), fy(size_t(nx) * size_t(nz), 1e30f);
        auto raster = [&](const ElAmTri &t, auto &&fn) {
            const float ax = t.v[0][0], az = t.v[0][2], bx = t.v[1][0], bz = t.v[1][2], cx = t.v[2][0], cz = t.v[2][2];
            const float area = (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
            if (area == 0) return;
            int i0, k0, i1, k1;
            cell(std::min({ ax, bx, cx }), std::min({ az, bz, cz }), &i0, &k0);
            cell(std::max({ ax, bx, cx }), std::max({ az, bz, cz }), &i1, &k1);
            i0 = std::max(i0, 0); k0 = std::max(k0, 0); i1 = std::min(i1, nx - 1); k1 = std::min(k1, nz - 1);
            for (int kz = k0; kz <= k1; kz++)
                for (int ix = i0; ix <= i1; ix++) {
                    const float px = x0 + (float(ix) + 0.5f) * cs, pz = z0 + (float(kz) + 0.5f) * cs;
                    const float w0 = ((bx - px) * (cz - pz) - (bz - pz) * (cx - px)) / area;
                    const float w1 = ((cx - px) * (az - pz) - (cz - pz) * (ax - px)) / area;
                    if (w0 < 0 || w1 < 0 || w0 + w1 > 1) continue;
                    fn(size_t(kz) * size_t(nx) + size_t(ix), w0 * t.v[0][1] + w1 * t.v[1][1] + (1 - w0 - w1) * t.v[2][1]);
                }
        };
        for (const ElAmTri &t : p.tris) raster(t, [&](size_t c, float y) { fy[c] = std::min(fy[c], y); });
        // a water object much smaller than the area (a pond, a pool, a river: not the sea under everything) is water
        // wherever no floor is over it, holes in the floor too
        std::unordered_map<uint16_t, std::array<float, 4>> box;
        for (const ElAmTri &t : p.water)
            for (int j = 0; j < 3; j++) {
                auto it = box.emplace(t.obj, std::array<float, 4>{ 1e30f, 1e30f, -1e30f, -1e30f }).first;
                it->second[0] = std::min(it->second[0], t.v[j][0]); it->second[2] = std::max(it->second[2], t.v[j][0]);
                it->second[1] = std::min(it->second[1], t.v[j][2]); it->second[3] = std::max(it->second[3], t.v[j][2]);
            }
        std::vector<uint8_t> local(size_t(nx) * size_t(nz), 0);
        for (const ElAmTri &t : p.water) {
            const std::array<float, 4> &bb = box[t.obj];
            const bool small = (bb[2] - bb[0]) * (bb[3] - bb[1]) < 0.15f * w * h;
            raster(t, [&](size_t c, float y) {
                wy[c] = std::max(wy[c], y);
                if (small) local[c] = 1;
            });
        }
        // the open: cells without floor, reached from the grid's edge
        std::vector<uint8_t> open(size_t(nx) * size_t(nz), 0);
        std::vector<int> q;
        for (int z = 0; z < nz; z++)
            for (int x = 0; x < nx; x++)
                if ((x == 0 || z == 0 || x == nx - 1 || z == nz - 1) && tri_of[size_t(z * nx + x)] < 0) {
                    open[size_t(z * nx + x)] = 1;
                    q.push_back(z * nx + x);
                }
        for (size_t k = 0; k < q.size(); k++) {
            const int c = q[k], x = c % nx, z = c / nx;
            const int nb[4][2] = { { x - 1, z }, { x + 1, z }, { x, z - 1 }, { x, z + 1 } };
            for (const auto &v : nb) {
                if (v[0] < 0 || v[1] < 0 || v[0] >= nx || v[1] >= nz) continue;
                const int d = v[1] * nx + v[0];
                if (open[size_t(d)] || tri_of[size_t(d)] >= 0) continue;
                open[size_t(d)] = 1;
                q.push_back(d);
            }
        }
        for (int z = 0; z < nz; z++) {
            int run = -1;
            for (int x = 0; x <= nx; x++) {
                bool w = false;
                if (x < nx) {
                    const size_t c = size_t(z) * size_t(nx) + size_t(x);
                    w = wy[c] > -1e29f && ((tri_of[c] < 0 && (open[c] || local[c])) || (tri_of[c] >= 0 && fy[c] < wy[c] - 1.0f));
                }
                if (w && run < 0) run = x;
                if (!w && run >= 0) {
                    p.wet.push_back({ x0 + float(run) * cs, z0 + float(z) * cs, x0 + float(x) * cs, z0 + float(z + 1) * cs });
                    run = -1;
                }
            }
        }
    }
    make_edges(p);
}

void prepare(Plan &p) {
    const size_t n = p.tris.size(), nw = 0; // (water: reach()'s runs, over the floor)
    // the order: floor and water triangles together, lowest first (a higher floor over a lower one, water over the
    // floor under it, a floor above the water over the water: the sea under the island)
    p.order.resize(n + nw);
    p.slope.resize(n);
    std::vector<float> mid(n + nw);
    for (size_t i = 0; i < nw; i++) {
        const ElAmTri &t = p.water[i];
        p.order[n + i] = uint32_t(n + i);
        mid[n + i] = std::max({ t.v[0][1], t.v[1][1], t.v[2][1] }) + 0.5f;
    }
    // the light: from the upper left of the map (north and +x, the screen's left), high
    const float lx = 0.45f, ly = 0.8f, lz = 0.4f;
    for (size_t i = 0; i < n; i++) {
        const ElAmTri &t = p.tris[i];
        p.order[i] = uint32_t(i);
        mid[i] = (t.v[0][1] + t.v[1][1] + t.v[2][1]) / 3.0f;
        const float ax = t.v[1][0] - t.v[0][0], ay = t.v[1][1] - t.v[0][1], az = t.v[1][2] - t.v[0][2];
        const float bx = t.v[2][0] - t.v[0][0], by = t.v[2][1] - t.v[0][1], bz = t.v[2][2] - t.v[0][2];
        float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 0) nx /= len, ny /= len, nz /= len;
        if (ny < 0) nx = -nx, ny = -ny, nz = -nz;
        const float l = (nx * lx + ny * ly + nz * lz) / ly;
        p.slope[i] = std::clamp(1.0f + (l - 1.0f) * 0.6f, 0.6f, 1.2f);
    }
    std::stable_sort(p.order.begin(), p.order.end(), [&](uint32_t a, uint32_t b) { return mid[a] < mid[b]; });
    p.reach.assign(n, 1);
    p.reach_done = false;
    std::memcpy(p.reach_box, p.bounds, sizeof p.reach_box);
    make_edges(p);
}

// ---- the view ------------------------------------------------------------------------------------

bool full = false;          // the full map is open
bool fitted = false;        // its view was fitted since it opened
float view_cx, view_cz;     // the world point at the centre of the full map's area
float view_zoom = 1.0f;     // times the fitted scale
float fit_scale = 1.0f;     // px per unit at zoom 1 (the area's at open)
float fit_box[4];           // what the fit showed (the pan stays inside it)
bool dragging = false;
float mouse_x = -1, mouse_y = -1; // the cursor (ImGui's units)
float wheel_acc = 0;
bool right_click = false;

// the game's picture in ImGui's units (x0, y0, x1, y1)
bool game_rect(float *r) {
    ImGuiIO &io = ImGui::GetIO();
    float px[4];
    if (renderer() && renderer()->picture_rect(px) && px[2] - px[0] >= 32 && px[3] - px[1] >= 32) {
        const float sx = io.DisplayFramebufferScale.x > 0 ? io.DisplayFramebufferScale.x : 1.0f;
        const float sy = io.DisplayFramebufferScale.y > 0 ? io.DisplayFramebufferScale.y : 1.0f;
        r[0] = px[0] / sx; r[1] = px[1] / sy; r[2] = px[2] / sx; r[3] = px[3] / sy;
        return true;
    }
    r[0] = r[1] = 0;
    r[2] = io.DisplaySize.x;
    r[3] = io.DisplaySize.y;
    return r[2] > 0 && r[3] > 0;
}

// where the player is drawn: between the two recordings the last picture was made from, as the picture put them
Sample player_now(bool smooth) {
    Sample s = newest;
    if (!smooth) return s;
    const render::NativeDraw::Shown &sh = render::native_draw().shown_frames();
    if (!sh.valid) return s;
    const Sample *a = find_sample(sh.a), *b = find_sample(sh.b);
    if (!a) return s;
    if (!b || b == a || b->area != a->area || sh.t <= 0) return *a;
    const float dx = b->x - a->x, dz = b->z - a->z;
    if (dx * dx + dz * dz > 200.0f * 200.0f) return sh.t < 1 ? *a : *b; // a jump (a warp): no slide across the map
    Sample r = *b;
    const float t = sh.t;
    r.x = a->x + dx * t;
    r.y = a->y + (b->y - a->y) * t;
    r.z = a->z + dz * t;
    float d = std::fmod(b->rot - a->rot, 6.2831853f);
    if (d > 3.1415927f) d -= 6.2831853f;
    if (d < -3.1415927f) d += 6.2831853f;
    r.rot = a->rot + d * t;
    return r;
}

struct Xform {
    float ox, oy;   // the screen point of the world point (cx, cz)
    float cx, cz;
    float s;        // px per unit
    ImVec2 at(float x, float z) const { return ImVec2(ox + MX * (x - cx) * s, oy - (z - cz) * s); }
    float wx(float sx) const { return cx + (sx - ox) / (MX * s); }
    float wz(float sy) const { return cz - (sy - oy) / s; }
};

ImU32 rgba(float r, float g, float b, float a) {
    auto c = [](float v) { return int(std::clamp(v, 0.0f, 255.0f) + 0.5f); };
    return IM_COL32(c(r), c(g), c(b), c(a));
}

// The floor: per corner, darker below the player and lighter above (a step every 25 units would band; this is
// smooth, 1 at the player's height, 0.6 a long way down, 1.25 a long way up), times the triangle's light
ImU32 floor_color(const ElAmTri &t, int corner, float slope, float py, float alpha, bool reachable) {
    const float d = std::clamp((t.v[corner][1] - py) / 300.0f, -1.0f, 1.0f);
    const float k = (d < 0 ? 1.0f + 0.4f * d : 1.0f + 0.25f * d) * slope;
    if (!reachable) { // out of reach: grey, faint
        const float g = 34.0f * slope;
        return rgba(g, g, g * 1.05f, 255.0f * 0.6f * alpha);
    }
    return rgba(FLOOR_R * k, FLOOR_G * k, FLOOR_B * k, 255.0f * alpha);
}

void draw_floor(ImDrawList *dl, const Xform &x, float py, float alpha) {
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    const ImU32 water_c = rgba(50, 88, 140, 255.0f * 0.85f * alpha);
    const size_t n = plan.order.size();
    for (size_t first = 0; first < n; first += 4096) {
        const size_t count = std::min<size_t>(4096, n - first);
        dl->PrimReserve(int(count * 3), int(count * 3));
        for (size_t k = 0; k < count; k++) {
            const uint32_t i = plan.order[first + k];
            const bool wet = i >= plan.tris.size();
            const ElAmTri &t = wet ? plan.water[i - plan.tris.size()] : plan.tris[i];
            const ImDrawIdx base = ImDrawIdx(dl->_VtxCurrentIdx);
            for (int j = 0; j < 3; j++) {
                dl->_VtxWritePtr->pos = x.at(t.v[j][0], t.v[j][2]);
                dl->_VtxWritePtr->uv = uv;
                dl->_VtxWritePtr->col = wet ? water_c : floor_color(t, j, plan.slope[i], py, alpha, plan.reach[i] != 0);
                dl->_VtxWritePtr++;
            }
            dl->_IdxWritePtr[0] = base;
            dl->_IdxWritePtr[1] = ImDrawIdx(base + 1);
            dl->_IdxWritePtr[2] = ImDrawIdx(base + 2);
            dl->_IdxWritePtr += 3;
            dl->_VtxCurrentIdx += 3;
        }
    }
}

void draw_wet(ImDrawList *dl, const Xform &x, float alpha) {
    const ImU32 c = rgba(50, 88, 140, 255.0f * 0.85f * alpha);
    for (const Edge &r : plan.wet) {
        const ImVec2 a = x.at(r.x0, r.z0), b = x.at(r.x1, r.z1);
        dl->AddRectFilled(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), c);
    }
}

void draw_lines(ImDrawList *dl, const Xform &x, float wall_w, float edge_w, float alpha, ImU32 wall_c = COL_WALL,
                ImU32 edge_c = COL_EDGE) {
    auto fade = [alpha](ImU32 c) { return (c & 0x00FFFFFFu) | (ImU32(float(c >> 24) * alpha) << 24); };
    for (const Edge &e : plan.edges) dl->AddLine(x.at(e.x0, e.z0), x.at(e.x1, e.z1), fade(edge_c), edge_w);
    for (const ElAmWall &w : plan.walls) dl->AddLine(x.at(w.x0, w.z0), x.at(w.x1, w.z1), fade(wall_c), wall_w);
}

// The player: KF's dart (tip, left wing, notch, right wing), light with a black outline
void draw_arrow(ImDrawList *dl, ImVec2 p, float rot, float size, float alpha) {
    const float fx = MX * std::sin(rot), fy = -std::cos(rot); // facing (sin rot, cos rot) in (x, z), on screen
    const float sx = -fy, sy = fx;                              // its side
    ImVec2 pts[4] = {
        ImVec2(p.x + fx * size, p.y + fy * size),
        ImVec2(p.x - fx * 0.7f * size + sx * 0.65f * size, p.y - fy * 0.7f * size + sy * 0.65f * size),
        ImVec2(p.x - fx * 0.45f * size, p.y - fy * 0.45f * size),
        ImVec2(p.x - fx * 0.7f * size - sx * 0.65f * size, p.y - fy * 0.7f * size - sy * 0.65f * size),
    };
    const ImU32 fill = (COL_ARROW & 0x00FFFFFFu) | (ImU32(255 * alpha) << 24);
    dl->AddConcavePolyFilled(pts, 4, fill);
    dl->AddPolyline(pts, 4, (COL_OUTLINE & 0x00FFFFFFu) | (ImU32(255 * alpha) << 24), ImDrawFlags_Closed,
                    std::max(1.5f, size * 0.12f));
}

// ---- the tests' pictures (ELTALE_AUTOMAP_SHOTS, with the game's truth view) ------------------------

const bool shots = std::getenv("ELTALE_AUTOMAP_SHOTS") != nullptr;
uint32_t shot_latched; // flagged shots not made yet
int shot_map_id, shot_sub_id;
uint32_t shot_kind;    // the picture being made (EL_AM_PLAYER_SHOT_*), until the renderer wrote it
char shot_name[64];

// The reachable floor, once the player is known to be in the plan (the test pictures: from the exits alone, the tour
// leaves the player where the last area had them). Seeds: the player, and a point just before each exit that wants a
// facing (the way the player comes to it: behind the box, against the angle they must face).
void ensure_reach() {
    if (!plan.valid() || plan.reach_done) return;
    const bool player_here = have_sample && newest.area == plan.serial;
    if (!shots && !player_here) return;
    std::vector<std::pair<float, float>> seeds;
    if (!shots) seeds.push_back({ newest.x, newest.z });
    for (const ElAmExit &e : plan.exits) {
        if (e.flags & 2) continue;
        const float d = e.half_d + 20.0f;
        seeds.push_back({ e.x - std::sin(e.angle) * d, e.z - std::cos(e.angle) * d });
    }
    reach(plan, seeds);
}

bool in_play() { return have_sample && plan.valid() && (newest.flags & EL_AM_PLAYER_PLAY) && newest.area == plan.serial; }

void fit(const float *area) {
    // the plan's bounds, with a margin of 4%; small rooms are not blown up past a sixth of the picture's height a
    // hundred units
    const float *b = plan.reach_box;
    const float w = std::max(b[2] - b[0], 50.0f) * 1.08f;
    const float h = std::max(b[3] - b[1], 50.0f) * 1.08f;
    const float aw = area[2] - area[0], ah = area[3] - area[1];
    const float gh_cap = ah / 250.0f; // at most: 250 units across the area's height (a small room)
    fit_scale = std::min({ aw / w, ah / h, gh_cap });
    view_cx = (b[0] + b[2]) * 0.5f;
    view_cz = (b[1] + b[3]) * 0.5f;
    std::memcpy(fit_box, plan.bounds, sizeof fit_box);
    view_zoom = 1.0f;
    fitted = true;
}

void draw_text(ImDrawList *dl, float size, ImVec2 p, ImU32 c, const char *text) {
    dl->AddText(ImGui::GetFont(), size, p, c, text);
}

// ELTALE_AUTOMAP_DUMP=1 (a test, tools/automap_test.py, with ELTALE_PRESENT_DUMP): a picture each time the full map's
// view changes (opened, zoomed, dragged, fitted again): automap_full_N.png, and the minimap once, 100 ticks into an
// area: automap_minimap.png. The arrow is then the newest tick's (no picture between: the same pictures each run).
const bool dump_test = std::getenv("ELTALE_AUTOMAP_DUMP") != nullptr;
int view_changes, view_dumped;
uint32_t samples_on_plan;
bool minimap_dumped;

void draw_full(ImDrawList *dl, const float *g) {
    const float gw = g[2] - g[0], gh = g[3] - g[1];
    const float ts = std::max(14.0f, gh / 34.0f);
    const float legend_w = gh * 0.30f;
    const float area[4] = { g[0] + gh * 0.04f, g[1] + gh * 0.08f, g[2] - legend_w - gh * 0.04f, g[1] + gh * 0.94f };
    (void)gw;
    if (!fitted) fit(area);

    // the input since the last picture: the wheel zooms about the point under the cursor, a drag moves the map
    const bool inside = mouse_x >= area[0] && mouse_x < area[2] && mouse_y >= area[1] && mouse_y < area[3];
    const float ax = (area[0] + area[2]) * 0.5f, ay = (area[1] + area[3]) * 0.5f;
    Xform x{ ax, ay, view_cx, view_cz, fit_scale * view_zoom };
    if (wheel_acc != 0) {
        const float steps = std::clamp(wheel_acc, -4.0f, 4.0f);
        wheel_acc = 0;
        const float zoom = std::clamp(view_zoom * std::pow(1.25f, steps), 1.0f, 24.0f);
        const float px = inside ? mouse_x : ax, py = inside ? mouse_y : ay;
        const float wx = x.wx(px), wz = x.wz(py);
        view_zoom = zoom;
        x.s = fit_scale * view_zoom;
        // keep (wx, wz) under (px, py)
        view_cx = wx - (px - ax) / (MX * x.s);
        view_cz = wz + (py - ay) / x.s;
    }
    if (right_click) {
        right_click = false;
        fit(area);
    }
    view_cx = std::clamp(view_cx, std::min(fit_box[0], fit_box[2]), std::max(fit_box[0], fit_box[2]));
    view_cz = std::clamp(view_cz, std::min(fit_box[1], fit_box[3]), std::max(fit_box[1], fit_box[3]));
    x = Xform{ ax, ay, view_cx, view_cz, fit_scale * view_zoom };

    const float bg = shots ? 1.0f : settings.map.background / 100.0f;
    dl->AddRectFilled(ImVec2(g[0], g[1]), ImVec2(g[2], g[3]), rgba(6, 5, 4, 255 * bg));
    const Sample p = player_now(!shots && !dump_test);
    if (dump_test && view_changes != view_dumped) {
        view_dumped = view_changes;
        char name[32];
        std::snprintf(name, sizeof name, "automap_full_%d", view_changes);
        renderer()->request_dump(name);
        std::printf("P1_AUTOMAP_VIEW n=%d map=%d sub=%d zoom=%.3f centre=%.1f,%.1f\n", view_changes, plan.map, plan.sub,
                    view_zoom, view_cx, view_cz);
        std::fflush(stdout);
    }
    dl->PushClipRect(ImVec2(area[0], area[1]), ImVec2(area[2], area[3]), true);
    draw_floor(dl, x, p.y, 1.0f);
    draw_wet(dl, x, 1.0f);
    const float wall_w = std::clamp(x.s * 3.0f, 1.0f, gh / 220.0f), edge_w = std::clamp(x.s * 1.5f, 1.0f, gh / 400.0f);
    draw_lines(dl, x, wall_w, edge_w, 1.0f);
    if (p.area == plan.serial)
        draw_arrow(dl, x.at(p.x, p.z), p.rot, std::max(9.0f, std::min(gh / 45.0f, x.s * 20.0f)), 1.0f);
    dl->PopClipRect();

    // the title, the legend and the help
    char title[64];
    std::snprintf(title, sizeof title, "Map %d, area %d", plan.map, plan.sub);
    draw_text(dl, ts * 1.4f, ImVec2(g[0] + gh * 0.04f, g[1] + gh * 0.02f), COL_TITLE, title);
    float lx = g[2] - legend_w, ly = g[1] + gh * 0.1f;
    const float sw = ts * 1.1f;
    auto swatch = [&](ImU32 c, const char *label) {
        dl->AddRectFilled(ImVec2(lx, ly + ts * 0.15f), ImVec2(lx + sw, ly + ts * 0.15f + sw * 0.7f), c);
        draw_text(dl, ts, ImVec2(lx + sw * 1.4f, ly), COL_HELP, label);
        ly += ts * 1.5f;
    };
    auto line = [&](ImU32 c, float w, const char *label) {
        dl->AddLine(ImVec2(lx, ly + ts * 0.5f), ImVec2(lx + sw, ly + ts * 0.5f), c, w);
        draw_text(dl, ts, ImVec2(lx + sw * 1.4f, ly), COL_HELP, label);
        ly += ts * 1.5f;
    };
    swatch(rgba(FLOOR_R * 1.2f, FLOOR_G * 1.2f, FLOOR_B * 1.2f, 255), "Floor above you");
    swatch(rgba(FLOOR_R, FLOOR_G, FLOOR_B, 255), "Floor");
    swatch(rgba(FLOOR_R * 0.65f, FLOOR_G * 0.65f, FLOOR_B * 0.65f, 255), "Floor below you");
    swatch(rgba(34, 34, 36, 255 * 0.6f), "Out of reach");
    swatch(rgba(50, 88, 140, 255 * 0.85f), "Water");
    line(COL_WALL, std::max(2.0f, gh / 300.0f), "Wall");
    line(COL_EDGE, std::max(1.5f, gh / 500.0f), "Ledge or drop");
    draw_arrow(dl, ImVec2(lx + sw * 0.5f, ly + ts * 0.55f), 0.0f, ts * 0.55f, 1.0f);
    draw_text(dl, ts, ImVec2(lx + sw * 1.4f, ly), COL_HELP, "You");
    ly += ts * 2.2f;
    std::string key = "M";
    for (int s = 0; s < BIND_SLOTS - 1; s++)
        if (settings.controls.bind[ACT_MAP][s]) { key = binding_label(settings.controls.bind[ACT_MAP][s]); break; }
    const std::string help = "Wheel: zoom\nDrag: move\nRight click: fit\n" + key + " or Esc: close";
    draw_text(dl, ts * 0.85f, ImVec2(lx, ly), COL_HELP, help.c_str());
    // north
    draw_text(dl, ts * 1.2f, ImVec2(area[2] - ts * 1.6f, area[1]), COL_NORTH, "N");
}

void draw_minimap(ImDrawList *dl, const float *g) {
    const float gh = g[3] - g[1];
    const MapSettings &m = settings.map;
    const float side = gh * m.size / 100.0f, margin = gh * 0.025f;
    const int corner = m.corner;
    const bool right = corner == 0 || corner == 1, bottom = corner == 1 || corner == 3;
    // (the compass is at the top right, the meters at the top left: a top map sits below them)
    const float x0 = right ? g[2] - margin - side : g[0] + margin;
    const float y0 = bottom ? g[3] - margin - side : g[1] + gh * (right ? 0.27f : 0.24f);
    const float a = m.opacity / 100.0f;
    const ImVec2 p0(x0, y0), p1(x0 + side, y0 + side);
    const float round = side * 0.04f;
    dl->AddRectFilled(p0, p1, rgba(10, 9, 8, 255 * 0.75f * a / 0.85f), round);
    const Sample p = player_now(!dump_test);
    if (dump_test && !minimap_dumped && samples_on_plan >= 100) {
        minimap_dumped = true;
        renderer()->request_dump("automap_minimap");
    }
    const float reach = m.reach * 100.0f;
    Xform x{ x0 + side * 0.5f, y0 + side * 0.5f, p.x, p.z, side * 0.5f / reach };
    dl->PushClipRect(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), true);
    draw_floor(dl, x, p.y, a);
    draw_wet(dl, x, a);
    draw_lines(dl, x, std::clamp(x.s * 3.0f, 1.0f, side / 90.0f), std::clamp(x.s * 1.5f, 1.0f, side / 160.0f), a);
    draw_arrow(dl, ImVec2(x0 + side * 0.5f, y0 + side * 0.5f), p.rot, std::max(5.0f, side * 0.045f), a);
    dl->PopClipRect();
    dl->AddRect(p0, p1, rgba(200, 180, 130, 255 * 0.8f * a), round, 0, 1.5f);
    const float ns = std::max(12.0f, side * 0.08f);
    draw_text(dl, ns, ImVec2(x0 + side * 0.5f - ns * 0.3f, y0 + 2), (COL_NORTH & 0x00FFFFFFu) | (ImU32(255 * a) << 24), "N");
}

// The plan beside the test pictures (tools/automap_path.py): am_MM_SS_plan.json in ELTALE_PRESENT_DUMP's folder
void write_plan_json(const float *g) {
    const char *dir = std::getenv("ELTALE_PRESENT_DUMP");
    if (!dir || !*dir) return;
    char path[1024];
    std::snprintf(path, sizeof path, "%s/am_%02d_%02d_plan.json", dir, shot_map_id, shot_sub_id);
    FILE *f = std::fopen(path, "w");
    if (!f) return;
    std::fprintf(f, "{\"map\": %d, \"sub\": %d, \"rect\": [%.1f, %.1f, %.1f, %.1f],\n", plan.map, plan.sub, g[0], g[1], g[2], g[3]);
    std::fprintf(f, " \"bounds\": [%.3f, %.3f, %.3f, %.3f], \"reach_box\": [%.3f, %.3f, %.3f, %.3f],\n", plan.bounds[0],
                 plan.bounds[1], plan.bounds[2], plan.bounds[3], plan.reach_box[0], plan.reach_box[1], plan.reach_box[2],
                 plan.reach_box[3]);
    std::fprintf(f, " \"tris\": %zu, \"reachable\": %zu, \"water\": %zu, \"edges\": %zu,\n", plan.tris.size(),
                 size_t(std::count(plan.reach.begin(), plan.reach.end(), uint8_t(1))), plan.water.size(), plan.edges.size());
    std::fprintf(f, " \"walls\": [");
    for (size_t i = 0; i < plan.walls.size(); i++) {
        const ElAmWall &w = plan.walls[i];
        std::fprintf(f, "%s[%.3f, %.3f, %.3f, %.3f]", i ? ", " : "", w.x0, w.z0, w.x1, w.z1);
    }
    std::fprintf(f, "]}\n");
    std::fclose(f);
}

// The map's lines over the truth view, framed as nd_automap.c frames it (a test)
void draw_over(ImDrawList *dl, const float *g) {
    const float gw = g[2] - g[0], gh = g[3] - g[1], aspect = gw / gh;
    const float cx = (plan.bounds[0] + plan.bounds[2]) * 0.5f, cz = (plan.bounds[1] + plan.bounds[3]) * 0.5f;
    float half = std::max((plan.bounds[3] - plan.bounds[1]) * 0.5f, (plan.bounds[2] - plan.bounds[0]) * 0.5f / aspect) * 1.04f;
    half = std::max(half, 100.0f);
    const Xform x{ (g[0] + g[2]) * 0.5f, (g[1] + g[3]) * 0.5f, cx, cz, gh * 0.5f / half };
    // (bright, to stand out on the picture: walls magenta, floor edges yellow)
    draw_lines(dl, x, std::max(1.5f, gh / 400.0f), std::max(1.0f, gh / 700.0f), 1.0f, IM_COL32(255, 40, 255, 255),
               IM_COL32(255, 230, 0, 255));
    const Sample p = newest;
    draw_arrow(dl, x.at(p.x, p.z), p.rot, std::max(9.0f, gh / 45.0f), 0.8f);
}

} // namespace

// ---- the game's side -----------------------------------------------------------------------------

void receive_area(const ElAmArea *a) {
    if (!a || a->version != EL_AM_VERSION) return;
    if (a->serial <= plan.serial) { // (the game was reset: its numbers start again; the old samples are void)
        for (Sample &s : samples) s = Sample();
        have_sample = false;
    }
    plan.serial = a->serial;
    plan.map = a->map;
    plan.sub = a->sub;
    plan.flags = a->flags;
    std::memcpy(plan.bounds, a->bounds, sizeof plan.bounds);
    plan.y_min = a->y_min;
    plan.y_max = a->y_max;
    plan.tris.assign(a->tris, a->tris + a->tri_count);
    plan.walls.assign(a->walls, a->walls + a->wall_count);
    plan.exits.assign(a->exits, a->exits + a->exit_count);
    plan.markers.assign(a->markers, a->markers + a->marker_count);
    plan.water.assign(a->water, a->water + a->water_count);
    prepare(plan);
    fitted = false; // (a new area: the open map fits it)
}

void receive_player(const ElAmPlayer *p) {
    if (!p) return;
    Sample s;
    s.frame = p->frame;
    s.area = p->area_serial;
    s.flags = p->flags;
    s.x = p->x; s.y = p->y; s.z = p->z; s.rot = p->rot;
    samples[sample_next] = s;
    sample_next = (sample_next + 1) % SAMPLES;
    newest = s;
    if (p->flags & (EL_AM_PLAYER_SHOT_TRUTH | EL_AM_PLAYER_SHOT_OVER | EL_AM_PLAYER_SHOT_MAP)) {
        shot_latched |= p->flags & (EL_AM_PLAYER_SHOT_TRUTH | EL_AM_PLAYER_SHOT_OVER | EL_AM_PLAYER_SHOT_MAP);
        shot_map_id = plan.map; // (named for the area it was asked in)
        shot_sub_id = plan.sub;
    }
    have_sample = true;
    samples_on_plan = p->area_serial == plan.serial ? samples_on_plan + 1 : 0;
}

// ---- the host's side -----------------------------------------------------------------------------

void toggle() {
    view_changes++;
    if (full) { full = false; dragging = false; return; }
    if (!in_play()) return;
    full = true;
    fitted = false;
    wheel_acc = 0;
    right_click = false;
}

bool full_open() { return full && in_play(); }
bool holds_game() { return full_open() && settings.map.pause; }

bool event(const SDL_Event &e) {
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        mouse_x = e.motion.x;
        mouse_y = e.motion.y;
        if (dragging && full_open() && fit_scale > 0) {
            const float s = fit_scale * view_zoom;
            view_cx -= e.motion.xrel / (MX * s);
            view_cz += e.motion.yrel / s;
        }
        return false; // (the motion is the game's too: mouse look is off while the map is open)
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
        if (dragging) view_changes++;
        dragging = false;
    }
    if (!full_open()) return false;
    if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        view_changes++;
        wheel_acc += float(e.wheel.integer_y) * (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f);
        return true;
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        mouse_x = e.button.x;
        mouse_y = e.button.y;
        if (e.button.button == SDL_BUTTON_LEFT) dragging = true;
        if (e.button.button == SDL_BUTTON_RIGHT) right_click = true, view_changes++;
        return true;
    }
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_ESCAPE) {
        full = false;
        dragging = false;
        return true;
    }
    return false;
}

bool wants_overlay() {
    if (shots && (shot_kind || shot_latched))
        return true;
    if (!in_play()) return false;
    if (shots && (newest.flags & EL_AM_PLAYER_TRUTH)) return false;
    return full || settings.map.minimap;
}

void draw() {
    float g[4];
    if (!game_rect(g)) return;
    ensure_reach();
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    // (a test's pictures: once per flagged tick; the same picture again until the renderer has written it, as a
    // retrace with no picture due shows none)
    if (shots && have_sample && plan.valid() && newest.area == plan.serial) {
        // (latched as the samples came: a tick can pass with no picture built)
        if (!shot_kind && shot_latched) {
            shot_kind = (shot_latched & EL_AM_PLAYER_SHOT_TRUTH) ? EL_AM_PLAYER_SHOT_TRUTH
                      : (shot_latched & EL_AM_PLAYER_SHOT_OVER) ? EL_AM_PLAYER_SHOT_OVER : EL_AM_PLAYER_SHOT_MAP;
            shot_latched &= ~shot_kind;
            const char *what = (shot_kind & EL_AM_PLAYER_SHOT_TRUTH) ? "truth" : (shot_kind & EL_AM_PLAYER_SHOT_OVER) ? "over" : "map";
            std::snprintf(shot_name, sizeof shot_name, "am_%02d_%02d_%s", shot_map_id, shot_sub_id, what);
            renderer()->request_dump(shot_name);
            if (shot_kind & EL_AM_PLAYER_SHOT_MAP) {
                std::printf("P1_AUTOMAP_SHOT map=%d sub=%d tris=%zu walls=%zu edges=%zu\n", plan.map, plan.sub, plan.tris.size(),
                            plan.walls.size(), plan.edges.size());
                std::fflush(stdout);
            }
        } else if (shot_kind && !renderer()->dump_pending()) {
            shot_kind = 0; // (written)
        }
        if ((shot_kind & EL_AM_PLAYER_SHOT_OVER) && renderer()->dump_pending() && !std::strcmp(shot_name + std::strlen(shot_name) - 4, "over"))
            write_plan_json(g);
        if (shot_kind & EL_AM_PLAYER_SHOT_OVER) draw_over(dl, g);
        if (shot_kind & EL_AM_PLAYER_SHOT_MAP) {
            const bool was = full;
            full = true;
            fitted = false;
            draw_full(dl, g);
            full = was;
            fitted = false;
        }
        if (shot_kind) return;
    }
    if (!in_play()) return;
    if (shots && (newest.flags & (EL_AM_PLAYER_TRUTH | EL_AM_PLAYER_SHOT_OVER | EL_AM_PLAYER_SHOT_MAP))) return;
    if (full) draw_full(dl, g);
    else if (settings.map.minimap) draw_minimap(dl, g);
}

void settings_page() {
    MapSettings &m = settings.map;
    bool changed = false;
    ImGui::TextWrapped("The area map: the floor plan of the place you are in. Open it with the Map key (Controls page; M "
                       "by default). Wheel: zoom, drag: move, right click: fit, the key or Esc: close.");
    ImGui::Spacing();
    changed |= ImGui::Checkbox("Pause the game while the map is open", &m.pause);
    ImGui::TextDisabled("(the game makes its own music: paused, it is silent)");
    changed |= ImGui::SliderInt("Map background", &m.background, 40, 100, "%d%%");
    ImGui::Separator();
    changed |= ImGui::Checkbox("Minimap", &m.minimap);
    const char *corners[] = { "Top right", "Bottom right", "Top left", "Bottom left" };
    changed |= ImGui::Combo("Corner", &m.corner, corners, 4);
    changed |= ImGui::SliderInt("Size", &m.size, 10, 45, "%d%% of the height");
    int reach = m.reach * 100;
    if (ImGui::SliderInt("Reach", &reach, 300, 3000, "%d units")) {
        m.reach = std::clamp((reach + 50) / 100, 3, 30);
        changed = true;
    }
    changed |= ImGui::SliderInt("Opacity", &m.opacity, 20, 100, "%d%%");
    if (changed) save_settings();
}

} // namespace automap
} // namespace host

extern "C" ELHOST_API void ElHost_AutomapArea(const ElAmArea *area) { host::automap::receive_area(area); }
extern "C" ELHOST_API void ElHost_AutomapPlayer(const ElAmPlayer *player) { host::automap::receive_player(player); }
