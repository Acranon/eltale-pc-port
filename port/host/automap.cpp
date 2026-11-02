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
#include <cfloat>
#include <string>
#include <filesystem>
#include <map>
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
    // (v3) random battles: the regions and the encounter points the game sent
    std::vector<ElAmZone> zones;
    std::vector<std::pair<float, float>> points;
    // the reach grid (reach()), kept for the battle zones (and fog of war): cell (ix, iz) covers
    // [gx0 + ix * gcs, +gcs) x [gz0 + iz * gcs, +gcs)
    float gx0 = 0, gz0 = 0, gcs = 1;
    int gnx = 0, gnz = 0;
    std::vector<uint8_t> walk;       // the cells walked to (reachable floor)
    std::vector<uint8_t> blocked;    // the cells a wall passes through (fog of war's sight stops there)
    std::vector<int32_t> cell_tri;   // the floor triangle a cell's centre lies in (-1 none)
    std::vector<uint8_t> zone_cell;  // the battle zone a battle can start in from that cell: region + 1, 0 none
    std::vector<Edge> zone_runs;     // the zones' cells as rectangles (x0, z0, x1, z1), for drawing
    std::vector<Edge> zone_edges;    // their outline
    bool cell_of(float x, float z, int *ix, int *iz) const {
        if (gnx <= 0) return false;
        *ix = int(std::floor((x - gx0) / gcs));
        *iz = int(std::floor((z - gz0) / gcs));
        return *ix >= 0 && *iz >= 0 && *ix < gnx && *iz < gnz;
    }
    int zone_at(float x, float z) const { // the shaded zone at (x, z): region + 1, 0 none
        int ix, iz;
        return zone_cell.empty() || !cell_of(x, z, &ix, &iz) ? 0 : zone_cell[size_t(iz) * size_t(gnx) + size_t(ix)];
    }
    bool valid() const { return serial != 0; }
};

// The game's rule at an exact point (Battle_FindEncounterRegion, the first region holding it; and
// Battle_ChooseNearbyEncounterPoint, a point at squared distance 2500..8100): region + 1, 0 none
int zone_rule(const Plan &p, float x, float z) {
    int found = -1;
    for (size_t i = 0; i < p.zones.size(); i++) {
        const ElAmZone &r = p.zones[i];
        if (r.x0 <= x && r.z0 <= z && x - r.x0 <= r.x1 - r.x0 && z - r.z0 <= r.z1 - r.z0) { found = int(i); break; }
    }
    if (found < 0) return 0;
    for (const auto &pt : p.points) {
        const float dx = pt.first - x, dz = pt.second - z, d = dx * dx + dz * dz;
        if (d >= 2500.0f && d <= 8100.0f) return found + 1;
    }
    return 0;
}

// The battle zones on the reach grid: a reachable cell where some point satisfies the game's rule (the cell has a
// point 50..90 units from an encounter point, and it touches a region; its zone is the first region holding its
// centre, else the first it touches), so every place a battle can start is shaded, and at most a cell more; as
// rectangles of runs and an outline of merged cell sides
void make_zones(Plan &p) {
    p.zone_cell.clear();
    p.zone_runs.clear();
    p.zone_edges.clear();
    if (p.zones.empty() || p.points.empty() || p.gnx <= 0) return;
    const int nx = p.gnx, nz = p.gnz;
    const float cs = p.gcs;
    std::vector<uint8_t> ring(size_t(nx) * size_t(nz), 0);
    for (const auto &pt : p.points) {
        int i0, k0, i1, k1;
        p.cell_of(pt.first - 91.0f, pt.second - 91.0f, &i0, &k0);
        p.cell_of(pt.first + 91.0f, pt.second + 91.0f, &i1, &k1);
        i0 = std::max(i0, 0); k0 = std::max(k0, 0); i1 = std::min(i1, nx - 1); k1 = std::min(k1, nz - 1);
        for (int kz = k0; kz <= k1; kz++)
            for (int ix = i0; ix <= i1; ix++) {
                // the cell's nearest and farthest points from the encounter point: some point of the cell is 50..90
                // units away when [near, far] meets [50, 90] (the distance is continuous over the square)
                const float cx0 = p.gx0 + float(ix) * cs, cz0 = p.gz0 + float(kz) * cs, cx1 = cx0 + cs, cz1 = cz0 + cs;
                const float nxd = std::max({ cx0 - pt.first, 0.0f, pt.first - cx1 }), nzd = std::max({ cz0 - pt.second, 0.0f, pt.second - cz1 });
                const float fxd = std::max(std::fabs(pt.first - cx0), std::fabs(pt.first - cx1));
                const float fzd = std::max(std::fabs(pt.second - cz0), std::fabs(pt.second - cz1));
                if (nxd * nxd + nzd * nzd <= 8100.0f && fxd * fxd + fzd * fzd >= 2500.0f) ring[size_t(kz) * size_t(nx) + size_t(ix)] = 1;
            }
    }
    p.zone_cell.assign(size_t(nx) * size_t(nz), 0);
    for (int kz = 0; kz < nz; kz++)
        for (int ix = 0; ix < nx; ix++) {
            const size_t c = size_t(kz) * size_t(nx) + size_t(ix);
            if (!ring[c] || !p.walk[c]) continue;
            // the region: the first holding the cell's centre (the game's first match), else the first the cell
            // touches (a cell across a region's edge)
            const float x = p.gx0 + (float(ix) + 0.5f) * cs, z = p.gz0 + (float(kz) + 0.5f) * cs, h = cs * 0.5f;
            for (size_t i = 0; i < p.zones.size() && !p.zone_cell[c]; i++) {
                const ElAmZone &r = p.zones[i];
                if (r.x0 <= x && r.z0 <= z && x <= r.x1 && z <= r.z1) p.zone_cell[c] = uint8_t(i + 1);
            }
            for (size_t i = 0; i < p.zones.size() && !p.zone_cell[c]; i++) {
                const ElAmZone &r = p.zones[i];
                if (r.x0 <= x + h && r.z0 <= z + h && x - h <= r.x1 && z - h <= r.z1) p.zone_cell[c] = uint8_t(i + 1);
            }
        }
    auto id = [&](int ix, int iz) { return ix < 0 || iz < 0 || ix >= nx || iz >= nz ? 0 : p.zone_cell[size_t(iz) * size_t(nx) + size_t(ix)]; };
    for (int kz = 0; kz < nz; kz++) {
        int run = -1, rid = 0;
        for (int ix = 0; ix <= nx; ix++) {
            const int v = ix < nx ? id(ix, kz) : 0;
            if (run >= 0 && v != rid) {
                p.zone_runs.push_back({ p.gx0 + float(run) * cs, p.gz0 + float(kz) * cs, p.gx0 + float(ix) * cs, p.gz0 + float(kz + 1) * cs });
                run = -1;
            }
            if (v && run < 0) run = ix, rid = v;
        }
    }
    // the outline: sides between cells of different zones (or a zone and none), merged along their line
    for (int bx = 0; bx <= nx; bx++) { // vertical sides at x = gx0 + bx * cs
        int run = -1;
        for (int kz = 0; kz <= nz; kz++) {
            const bool side = kz < nz && id(bx - 1, kz) != id(bx, kz);
            if (side && run < 0) run = kz;
            if (!side && run >= 0) {
                p.zone_edges.push_back({ p.gx0 + float(bx) * cs, p.gz0 + float(run) * cs, p.gx0 + float(bx) * cs, p.gz0 + float(kz) * cs });
                run = -1;
            }
        }
    }
    for (int bz = 0; bz <= nz; bz++) {
        int run = -1;
        for (int ix = 0; ix <= nx; ix++) {
            const bool side = ix < nx && id(ix, bz - 1) != id(ix, bz);
            if (side && run < 0) run = ix;
            if (!side && run >= 0) {
                p.zone_edges.push_back({ p.gx0 + float(run) * cs, p.gz0 + float(bz) * cs, p.gx0 + float(ix) * cs, p.gz0 + float(bz) * cs });
                run = -1;
            }
        }
    }
}
Plan plan;

// the player, by the tick's frame number (the native renderer's recordings carry the same)
struct Sample {
    uint32_t frame = 0, area = 0, flags = 0;
    float x = 0, y = 0, z = 0, rot = 0;
    std::vector<ElAmMarker> marks; // the tick's marks (doors, people, chests...)
    float travel = 0;              // (v3) the battle roll: battle_travel_accumulator
    uint16_t roll_limit = 0, danger = 0;
    int16_t zone = -1;
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
    p.gx0 = x0; p.gz0 = z0; p.gcs = cs; p.gnx = nx; p.gnz = nz;
    p.walk = seen;
    p.blocked = blocked;
    p.cell_tri = tri_of;
    if (queue.empty()) // (nothing to start from: all the floor)
        for (size_t c = 0; c < p.walk.size(); c++) p.walk[c] = tri_of[c] >= 0;
    make_zones(p);

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
bool zones_mouse = false;     // the full map's "Battle zones" button is held with the left button
float zb_rect[4];             // where that button was last drawn
bool zb_shown = false;
const bool zones_test = std::getenv("ELTALE_AUTOMAP_ZONES") != nullptr; // (a test: shown as if held)

// the battle zones are shown: the cheat on, the area has some, and the button or the key held
bool zones_held() { return zones_mouse || action_held(ACT_MAP_ZONES); }
bool zones_on() { return settings.map.cheat_zones && !plan.zone_runs.empty() && (zones_held() || zones_test); }

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

bool fog_on();
extern std::vector<uint8_t> fog_tri;
struct FogCell;
void draw_fog_cells(ImDrawList *dl, const Xform &x, float py, float alpha);

void draw_floor(ImDrawList *dl, const Xform &x, float py, float alpha) {
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    const ImU32 water_c = rgba(50, 88, 140, 255.0f * 0.85f * alpha);
    // (fog of war: the triangles all seen; those partly seen as their seen cells, after)
    const bool fog = fog_on() && fog_tri.size() == plan.tris.size();
    std::vector<uint32_t> shown;
    if (fog) {
        shown.reserve(plan.order.size());
        for (uint32_t i : plan.order)
            if (i >= plan.tris.size() || fog_tri[i] == 2) shown.push_back(i);
    }
    const std::vector<uint32_t> &order = fog ? shown : plan.order;
    const size_t n = order.size();
    for (size_t first = 0; first < n; first += 4096) {
        const size_t count = std::min<size_t>(4096, n - first);
        dl->PrimReserve(int(count * 3), int(count * 3));
        for (size_t k = 0; k < count; k++) {
            const uint32_t i = order[first + k];
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
    if (fog) draw_fog_cells(dl, x, py, alpha);
}

extern std::vector<Edge> fog_walls, fog_edges, fog_wet;

void draw_wet(ImDrawList *dl, const Xform &x, float alpha) {
    const ImU32 c = rgba(50, 88, 140, 255.0f * 0.85f * alpha);
    for (const Edge &r : fog_on() ? fog_wet : plan.wet) {
        const ImVec2 a = x.at(r.x0, r.z0), b = x.at(r.x1, r.z1);
        dl->AddRectFilled(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), c);
    }
}

void draw_lines(ImDrawList *dl, const Xform &x, float wall_w, float edge_w, float alpha, ImU32 wall_c = COL_WALL,
                ImU32 edge_c = COL_EDGE) {
    auto fade = [alpha](ImU32 c) { return (c & 0x00FFFFFFu) | (ImU32(float(c >> 24) * alpha) << 24); };
    if (fog_on()) { // (fog of war: cut to what has been seen)
        for (const Edge &e : fog_edges) dl->AddLine(x.at(e.x0, e.z0), x.at(e.x1, e.z1), fade(edge_c), edge_w);
        for (const Edge &w : fog_walls) dl->AddLine(x.at(w.x0, w.z0), x.at(w.x1, w.z1), fade(wall_c), wall_w);
        return;
    }
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

// The marks as the picture shows them: the newer recording's, people and the boss slid from where the older one had
// them (as the picture moves their models)
std::vector<ElAmMarker> marks_now(bool smooth) {
    if (!smooth) return newest.marks;
    const render::NativeDraw::Shown &sh = render::native_draw().shown_frames();
    if (!sh.valid) return newest.marks;
    const Sample *a = find_sample(sh.a), *b = find_sample(sh.b);
    if (!a) return newest.marks;
    if (!b || b == a || b->area != a->area || sh.t <= 0) return a->marks;
    std::vector<ElAmMarker> r = b->marks;
    for (ElAmMarker &m : r) {
        if (m.kind != EL_AM_MARK_PERSON && m.kind != EL_AM_MARK_BOSS) continue;
        for (const ElAmMarker &o : a->marks)
            if (o.id == m.id) {
                const float dx = m.x - o.x, dz = m.z - o.z;
                if (dx * dx + dz * dz < 200.0f * 200.0f) {
                    m.x = o.x + dx * sh.t;
                    m.y = o.y + (m.y - o.y) * sh.t;
                    m.z = o.z + dz * sh.t;
                }
                break;
            }
    }
    return r;
}

// ---- the marks: doors and ways out, people; the cheats' chests, spirits and bosses (KF3/KF4's icons) ----------

constexpr ImU32 COL_DOOR = IM_COL32(190, 120, 50, 255);
constexpr ImU32 COL_LOCKED = IM_COL32(210, 60, 50, 255);
constexpr ImU32 COL_SHUT = IM_COL32(120, 112, 100, 255);
constexpr ImU32 COL_EXIT = IM_COL32(250, 250, 255, 255);
constexpr ImU32 COL_PERSON = IM_COL32(255, 60, 170, 255);
constexpr ImU32 COL_CHEST = IM_COL32(230, 170, 60, 255);
constexpr ImU32 COL_SPIRIT = IM_COL32(190, 130, 255, 255); // (the game's sparkles are violet)
constexpr ImU32 COL_BOSS = IM_COL32(235, 50, 50, 255);
constexpr ImU32 COL_MARK_OUTLINE = IM_COL32(0, 0, 0, 205);
constexpr ImU32 COL_SIGN = IM_COL32(200, 160, 110, 255);
constexpr ImU32 COL_STORY = IM_COL32(130, 210, 255, 255);  // the ring round a person with a story line
constexpr ImU32 COL_GIVER = IM_COL32(255, 215, 80, 255);   // (cheat) a person who gives a story item
constexpr ImU32 COL_ZONE = IM_COL32(230, 45, 40, 255);     // (cheat) the battle zones
// the services (EL_AM_SVC_*): the inn's teal, the Wings' sky blue, the food's wheat, the MP remedies' lavender
constexpr ImU32 COL_SVC[EL_AM_SVC_COUNT] = { IM_COL32(255, 255, 255, 255), IM_COL32(70, 215, 175, 255),
                                             IM_COL32(120, 185, 255, 255), IM_COL32(250, 210, 110, 255),
                                             IM_COL32(190, 170, 255, 255) };
const char *const SVC_NAME[EL_AM_SVC_COUNT] = { "", "Inn", "Traveler's Wings", "Food", "MP remedy" };

ImU32 fade(ImU32 c, float a) { return (c & 0x00FFFFFFu) | (ImU32(float(c >> 24) * std::clamp(a, 0.0f, 1.0f)) << 24); }

// which marks are on (the Map page; `all` for the tests' pictures)
bool discovered(const ElAmMarker &m);
bool mark_shown(const ElAmMarker &m, bool all) {
    if (all) return true;
    const MapSettings &s = settings.map;
    if (!discovered(m)) return false;
    switch (m.kind) {
    case EL_AM_MARK_DOOR:
    case EL_AM_MARK_EXIT: return s.doors;
    case EL_AM_MARK_PERSON: return s.people;
    case EL_AM_MARK_CHEST: return s.cheat_chests;
    case EL_AM_MARK_SPIRIT: return s.cheat_spirits;
    case EL_AM_MARK_BOSS: return s.cheat_bosses;
    case EL_AM_MARK_SIGN: return s.signs;
    default: return false;
    }
}

// a person's or door's extras, as the Map page has them on (`all`: the tests' pictures)
bool service_shown(bool all) { return all || settings.map.services; }
bool story_shown(const ElAmMarker &m, bool all) { return (m.flags & EL_AM_MARK_STORY) && (all || settings.map.story); }
bool giver_shown(const ElAmMarker &m, bool all) { return (m.flags & EL_AM_MARK_KEY_GIVER) && (all || settings.map.cheat_givers); }
// the services a door leads to, shown
unsigned door_services(const ElAmMarker &m, bool all) {
    return (m.kind == EL_AM_MARK_DOOR || m.kind == EL_AM_MARK_EXIT) && service_shown(all) ? m.services : 0u;
}

// The hover text: the label and, when its mark is shown, the detail
std::string hover_text(const ElAmMarker &m, bool all) {
    std::string t = m.label;
    bool more = false;
    if (m.kind == EL_AM_MARK_PERSON)
        more = (m.service && service_shown(all)) || story_shown(m, all) || giver_shown(m, all);
    else if (door_services(m, all))
        more = true;
    if (more && m.detail[0]) t += std::string("\n") + m.detail;
    return t;
}

// A service's icon: a disc in its colour, a dark glyph (the inn's crescent moon, a pair of wings, a loaf, a drop)
void draw_service_icon(ImDrawList *dl, ImVec2 p, float k, int svc, float alpha) {
    if (svc <= 0 || svc >= EL_AM_SVC_COUNT) return;
    const ImU32 bg = fade(COL_SVC[svc], alpha), ink = fade(IM_COL32(25, 22, 18, 255), alpha);
    dl->AddCircleFilled(p, k, bg, 18);
    dl->AddCircle(p, k, fade(COL_MARK_OUTLINE, alpha), 18, 1.0f);
    switch (svc) {
    case EL_AM_SVC_INN: // a crescent moon (rest)
        dl->AddCircleFilled(p, k * 0.62f, ink, 16);
        dl->AddCircleFilled(ImVec2(p.x + k * 0.3f, p.y - k * 0.2f), k * 0.52f, bg, 16);
        break;
    case EL_AM_SVC_WINGS: { // two wings
        for (float sgn : { -1.0f, 1.0f }) {
            const ImVec2 a(p.x + sgn * k * 0.08f, p.y - k * 0.1f), b(p.x + sgn * k * 0.72f, p.y - k * 0.5f),
                c(p.x + sgn * k * 0.55f, p.y + k * 0.15f), d(p.x + sgn * k * 0.12f, p.y + k * 0.35f);
            dl->AddQuadFilled(a, b, c, d, ink);
        }
        break;
    }
    case EL_AM_SVC_FOOD: { // a loaf with its cuts
        const ImVec2 a(p.x - k * 0.62f, p.y - k * 0.32f), b(p.x + k * 0.62f, p.y + k * 0.36f);
        dl->AddRectFilled(a, b, ink, k * 0.32f);
        for (int i = -1; i <= 1; i++)
            dl->AddLine(ImVec2(p.x + i * k * 0.3f - k * 0.08f, p.y - k * 0.2f), ImVec2(p.x + i * k * 0.3f + k * 0.08f, p.y + k * 0.2f), bg,
                        std::max(1.0f, k * 0.12f));
        break;
    }
    case EL_AM_SVC_MP: { // a drop (dew, a remedy): a round bottom and a point on top
        const ImVec2 c(p.x, p.y + k * 0.18f);
        const float r = k * 0.42f;
        dl->AddCircleFilled(c, r, ink, 16);
        dl->AddTriangleFilled(ImVec2(p.x, p.y - k * 0.7f), ImVec2(c.x - r * 0.93f, c.y - r * 0.36f), ImVec2(c.x + r * 0.93f, c.y - r * 0.36f), ink);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.35f, c.y - r * 0.05f), r * 0.22f, bg, 10); // a glint
        break;
    }
    default: break;
    }
}

// the icons of every service in `bits`, side by side about p
void draw_service_icons(ImDrawList *dl, ImVec2 p, float k, unsigned bits, float alpha) {
    int n = 0;
    for (int s = 1; s < EL_AM_SVC_COUNT; s++) n += (bits >> s) & 1;
    float x = p.x - (float(n) - 1.0f) * k * 1.1f;
    for (int s = 1; s < EL_AM_SVC_COUNT; s++)
        if (bits & EL_AM_SVC_BIT(s)) { draw_service_icon(dl, ImVec2(x, p.y), k, s, alpha); x += k * 2.2f; }
}
// the first service's colour (a door's halo)
ImU32 service_color(unsigned bits) {
    for (int s = 1; s < EL_AM_SVC_COUNT; s++)
        if (bits & EL_AM_SVC_BIT(s)) return COL_SVC[s];
    return COL_SVC[0];
}

// an exit's way through and its side, on the screen (world (sin a, cos a) and (cos a, -sin a)), unit length
void exit_axes(const Xform &x, const ElAmMarker &m, ImVec2 *fwd, ImVec2 *side) {
    const ImVec2 o = x.at(0, 0), f = x.at(std::sin(m.angle), std::cos(m.angle)), sd = x.at(std::cos(m.angle), -std::sin(m.angle));
    *fwd = ImVec2((f.x - o.x) / x.s, (f.y - o.y) / x.s);
    *side = ImVec2((sd.x - o.x) / x.s, (sd.y - o.y) / x.s);
}

// a chevron pointing the way through
void draw_chevron(ImDrawList *dl, ImVec2 p, ImVec2 fwd, ImVec2 side, float k, ImU32 c, float alpha) {
    const ImVec2 tip(p.x + fwd.x * k, p.y + fwd.y * k);
    const ImVec2 l(p.x - fwd.x * k * 0.2f + side.x * k * 0.75f, p.y - fwd.y * k * 0.2f + side.y * k * 0.75f);
    const ImVec2 r(p.x - fwd.x * k * 0.2f - side.x * k * 0.75f, p.y - fwd.y * k * 0.2f - side.y * k * 0.75f);
    dl->AddTriangleFilled(tip, l, r, fade(c, alpha));
    dl->AddTriangle(tip, l, r, fade(COL_MARK_OUTLINE, alpha), 1.0f);
}

void draw_mark(ImDrawList *dl, const Xform &x, const ElAmMarker &m, float r, float alpha, bool all = false) {
    const ImVec2 p = x.at(m.x, m.z);
    const bool done = (m.flags & EL_AM_MARK_DONE) != 0;
    const unsigned svc = door_services(m, all);
    switch (m.kind) {
    case EL_AM_MARK_DOOR: {
        // a bar across the doorway, on the wall line (KF4's door); an exit to another map points the way out
        ImVec2 fwd, side;
        exit_axes(x, m, &fwd, &side);
        const float hw = std::max(m.half_w * x.s, r * 0.8f);
        const ImVec2 a(p.x - side.x * hw, p.y - side.y * hw), b(p.x + side.x * hw, p.y + side.y * hw);
        const ImU32 c = (m.flags & EL_AM_MARK_LOCKED) ? COL_LOCKED : (m.flags & EL_AM_MARK_SHUT) ? COL_SHUT : COL_DOOR;
        const float w = std::clamp(r * 0.55f, 3.0f, 9.0f);
        if (svc) { // a door to a service: a halo in its colour round the bar, its icon on this side of it
            dl->AddLine(a, b, fade(service_color(svc), alpha * 0.55f), w + 9.0f);
            dl->AddLine(a, b, fade(service_color(svc), alpha), w + 5.0f);
            const float k = std::clamp(r * 0.62f, 5.0f, 12.0f), off = w * 0.5f + k * 1.6f;
            draw_service_icons(dl, ImVec2(p.x - fwd.x * off, p.y - fwd.y * off), k, svc, alpha);
        }
        dl->AddLine(a, b, fade(COL_MARK_OUTLINE, alpha), w + 2.5f);
        dl->AddLine(a, b, fade(c, alpha), w);
        if (m.flags & EL_AM_MARK_LOCKED) dl->AddCircleFilled(p, w * 0.45f, fade(IM_COL32(20, 10, 8, 255), alpha), 8); // a keyhole
        if (m.flags & EL_AM_MARK_AWAY) {
            const float k = std::max(r * 0.7f, 5.0f);
            draw_chevron(dl, ImVec2(p.x + fwd.x * (w * 0.5f + k * 0.35f), p.y + fwd.y * (w * 0.5f + k * 0.35f)), fwd, side, k, COL_EXIT,
                         alpha);
        }
        break;
    }
    case EL_AM_MARK_EXIT: {
        // the box the player walks into (KF4's way to another map), a chevron the way through (white: another map)
        ImVec2 fwd, side;
        exit_axes(x, m, &fwd, &side);
        const float hw = std::max(m.half_w * x.s, r * 0.6f), hd = std::max(m.half_d * x.s, r * 0.3f);
        ImVec2 q[4] = {
            ImVec2(p.x - side.x * hw - fwd.x * hd, p.y - side.y * hw - fwd.y * hd),
            ImVec2(p.x + side.x * hw - fwd.x * hd, p.y + side.y * hw - fwd.y * hd),
            ImVec2(p.x + side.x * hw + fwd.x * hd, p.y + side.y * hw + fwd.y * hd),
            ImVec2(p.x - side.x * hw + fwd.x * hd, p.y - side.y * hw + fwd.y * hd),
        };
        const ImU32 c = (m.flags & EL_AM_MARK_LOCKED) ? COL_LOCKED : (m.flags & EL_AM_MARK_SHUT) ? COL_SHUT : COL_EXIT;
        if (svc) {
            dl->AddPolyline(q, 4, fade(service_color(svc), alpha), ImDrawFlags_Closed, std::max(3.0f, r * 0.45f));
            const float k = std::clamp(r * 0.62f, 5.0f, 12.0f), off = hd + k * 1.6f;
            draw_service_icons(dl, ImVec2(p.x - fwd.x * off, p.y - fwd.y * off), k, svc, alpha);
        }
        dl->AddConvexPolyFilled(q, 4, fade(c, alpha * 0.22f));
        dl->AddPolyline(q, 4, fade(c, alpha * 0.8f), ImDrawFlags_Closed, std::max(1.0f, r * 0.15f));
        const float k = std::clamp(std::min(hw, hd * 2.0f) * 0.8f, r * 0.45f, r * 1.1f);
        draw_chevron(dl, p, fwd, side, k, (m.flags & EL_AM_MARK_AWAY) ? COL_EXIT : COL_DOOR, alpha);
        break;
    }
    case EL_AM_MARK_PERSON: {
        const float k = r * 0.8f;
        const float rim = std::max(1.5f, k * 0.22f);
        // a ring round the disc: a story line (light blue), a story item to give (gold, a cheat)
        const bool giver = giver_shown(m, all), story = story_shown(m, all);
        if (giver || story) {
            const float rr = k + rim + std::max(2.0f, k * 0.3f);
            dl->AddCircle(p, rr, fade(COL_MARK_OUTLINE, alpha), 20, std::max(2.5f, k * 0.32f) + 2.0f);
            dl->AddCircle(p, rr, fade(giver ? COL_GIVER : COL_STORY, alpha), 20, std::max(2.5f, k * 0.32f));
        }
        dl->AddCircleFilled(p, k, fade(COL_PERSON, alpha), 16);
        dl->AddCircle(p, k, fade(IM_COL32(255, 255, 255, 255), alpha), 16, rim);
        dl->AddCircle(p, k + rim * 0.5f + 0.5f, fade(COL_MARK_OUTLINE, alpha), 16, 1.0f);
        if (giver) { // a key on the disc
            const float q = k * 0.5f;
            dl->AddCircle(ImVec2(p.x - q * 0.5f, p.y), q * 0.45f, fade(COL_GIVER, alpha), 10, std::max(1.5f, q * 0.3f));
            dl->AddLine(ImVec2(p.x - q * 0.05f, p.y), ImVec2(p.x + q * 1.1f, p.y), fade(COL_GIVER, alpha), std::max(1.5f, q * 0.3f));
            dl->AddLine(ImVec2(p.x + q * 0.8f, p.y), ImVec2(p.x + q * 0.8f, p.y + q * 0.5f), fade(COL_GIVER, alpha), std::max(1.5f, q * 0.3f));
        }
        if (m.service && service_shown(all)) // the service's badge at the upper right
            draw_service_icon(dl, ImVec2(p.x + k * 1.05f, p.y - k * 1.05f), std::max(5.0f, k * 0.8f), m.service, alpha);
        break;
    }
    case EL_AM_MARK_SIGN: {
        // a signpost: a board on a post
        const float k = r * 0.8f;
        const ImVec2 a(p.x - k * 0.85f, p.y - k * 0.75f), b(p.x + k * 0.85f, p.y + k * 0.05f);
        dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x, p.y + k * 0.9f), fade(COL_MARK_OUTLINE, alpha), std::max(2.0f, k * 0.3f) + 2.0f);
        dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x, p.y + k * 0.9f), fade(COL_SIGN, alpha), std::max(2.0f, k * 0.3f));
        dl->AddRectFilled(a, b, fade(COL_SIGN, alpha), k * 0.12f);
        dl->AddRect(a, b, fade(COL_MARK_OUTLINE, alpha), k * 0.12f, 0, 1.0f);
        dl->AddLine(ImVec2(a.x + k * 0.3f, p.y - k * 0.45f), ImVec2(b.x - k * 0.3f, p.y - k * 0.45f),
                    fade(IM_COL32(90, 60, 30, 255), alpha), std::max(1.0f, k * 0.12f));
        dl->AddLine(ImVec2(a.x + k * 0.3f, p.y - k * 0.22f), ImVec2(b.x - k * 0.5f, p.y - k * 0.22f),
                    fade(IM_COL32(90, 60, 30, 255), alpha), std::max(1.0f, k * 0.12f));
        break;
    }
    case EL_AM_MARK_CHEST: {
        const ImVec2 h(r * 0.8f, r * 0.58f);
        if (done) {
            dl->AddRectFilled(ImVec2(p.x - h.x, p.y - h.y), ImVec2(p.x + h.x, p.y + h.y), fade(IM_COL32(20, 17, 12, 220), alpha));
            dl->AddRect(ImVec2(p.x - h.x, p.y - h.y), ImVec2(p.x + h.x, p.y + h.y), fade(COL_CHEST, alpha * 0.85f), 0.0f, 0,
                        std::max(1.5f, r * 0.2f));
        } else {
            dl->AddRectFilled(ImVec2(p.x - h.x, p.y - h.y), ImVec2(p.x + h.x, p.y + h.y), fade(COL_CHEST, alpha));
            dl->AddLine(ImVec2(p.x - h.x, p.y - h.y * 0.15f), ImVec2(p.x + h.x, p.y - h.y * 0.15f),
                        fade(IM_COL32(120, 70, 20, 255), alpha), std::max(1.0f, r * 0.12f));
            dl->AddRect(ImVec2(p.x - h.x, p.y - h.y), ImVec2(p.x + h.x, p.y + h.y), fade(COL_MARK_OUTLINE, alpha), 0.0f, 0, 1.0f);
        }
        break;
    }
    case EL_AM_MARK_SPIRIT: {
        // a four-pointed sparkle; taken: its outline
        const float k = r * 0.95f, n = k * 0.32f;
        ImVec2 q[8];
        for (int i = 0; i < 8; i++) {
            const float a = float(i) * 0.78539816f, d = (i & 1) ? n : k;
            q[i] = ImVec2(p.x + std::sin(a) * d, p.y - std::cos(a) * d);
        }
        if (done) {
            dl->AddPolyline(q, 8, fade(COL_SPIRIT, alpha * 0.7f), ImDrawFlags_Closed, std::max(1.0f, r * 0.12f));
        } else {
            dl->AddConcavePolyFilled(q, 8, fade(COL_SPIRIT, alpha));
            dl->AddPolyline(q, 8, fade(COL_MARK_OUTLINE, alpha), ImDrawFlags_Closed, 1.0f);
            dl->AddCircleFilled(p, n * 0.6f, fade(IM_COL32(255, 255, 255, 255), alpha), 8);
        }
        break;
    }
    case EL_AM_MARK_BOSS: {
        // how near starts the fight (a faint ring, world-sized), and a red diamond with a light rim
        if (m.radius > 0) {
            const float rr = m.radius * x.s;
            dl->AddCircleFilled(p, rr, fade(COL_BOSS, alpha * 0.10f), 48);
            dl->AddCircle(p, rr, fade(COL_BOSS, alpha * 0.55f), 48, 1.5f);
        }
        const float k = r * 1.0f;
        const ImVec2 t(p.x, p.y - k), rt(p.x + k, p.y), b(p.x, p.y + k), lf(p.x - k, p.y);
        dl->AddQuadFilled(t, rt, b, lf, fade(COL_BOSS, alpha));
        dl->AddQuad(t, rt, b, lf, fade(IM_COL32(255, 235, 220, 255), alpha), std::max(1.5f, k * 0.15f));
        break;
    }
    default: break;
    }
}

// Every mark shown: the ways out first, then doors, chests and spirits, people and the boss on top
void draw_marks(ImDrawList *dl, const Xform &x, const std::vector<ElAmMarker> &marks, float r, float alpha, bool all,
                const float *clip) {
    static const uint16_t order[] = { EL_AM_MARK_EXIT, EL_AM_MARK_DOOR, EL_AM_MARK_SIGN, EL_AM_MARK_CHEST, EL_AM_MARK_SPIRIT,
                                      EL_AM_MARK_PERSON, EL_AM_MARK_BOSS };
    for (uint16_t kind : order)
        for (const ElAmMarker &m : marks) {
            if (m.kind != kind || !mark_shown(m, all)) continue;
            const ImVec2 p = x.at(m.x, m.z);
            const float pad = r * 3.0f + std::max({ m.half_w, m.half_d, m.radius }) * x.s;
            if (clip && (p.x < clip[0] - pad || p.y < clip[1] - pad || p.x > clip[2] + pad || p.y > clip[3] + pad)) continue;
            draw_mark(dl, x, m, r, alpha, all);
        }
}

// The battle zones (a cheat, while its button or key is held): translucent red over the cells where a battle can
// start, a red outline round them
void draw_zones(ImDrawList *dl, const Xform &x, float alpha, float line_w) {
    const ImU32 fill = fade(COL_ZONE, alpha * 0.30f), edge = fade(COL_ZONE, alpha * 0.9f);
    for (const Edge &r : plan.zone_runs) {
        const ImVec2 a = x.at(r.x0, r.z0), b = x.at(r.x1, r.z1);
        dl->AddRectFilled(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), fill);
    }
    for (const Edge &e : plan.zone_edges) dl->AddLine(x.at(e.x0, e.z0), x.at(e.x1, e.z1), edge, line_w);
}

// "Battle zone 2 of 4: ...", for a zone id (region + 1)
std::string zone_label(int id) {
    if (id <= 0 || size_t(id) > plan.zones.size()) return std::string();
    char head[48];
    std::snprintf(head, sizeof head, "Battle zone %d of %zu", id, plan.zones.size());
    const char *what = plan.zones[size_t(id - 1)].label;
    return std::string(head) + (what[0] ? std::string(":\n") + what : std::string());
}

// The mark under the mouse (screen point q): the nearest within `reach` pixels, or a door's bar or an exit's box
// under it; -1 none
int pick_mark(const Xform &x, const std::vector<ElAmMarker> &marks, ImVec2 q, float reach, bool all) {
    int best = -1;
    float best_d = reach * reach;
    for (size_t i = 0; i < marks.size(); i++) {
        const ElAmMarker &m = marks[i];
        if (!mark_shown(m, all)) continue;
        const ImVec2 p = x.at(m.x, m.z);
        float d = (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y);
        if (m.kind == EL_AM_MARK_DOOR || m.kind == EL_AM_MARK_EXIT) {
            // in the exit's own frame (world units): across and along the way through
            const float dx = x.wx(q.x) - m.x, dz = x.wz(q.y) - m.z;
            const float along = dx * std::sin(m.angle) + dz * std::cos(m.angle);
            const float across = dx * std::cos(m.angle) - dz * std::sin(m.angle);
            const float slack = reach * 0.5f / x.s;
            const float hd = m.kind == EL_AM_MARK_DOOR ? 0.0f : m.half_d;
            if (std::fabs(across) <= m.half_w + slack && std::fabs(along) <= hd + slack) d = std::min(d, best_d * 0.98f);
        }
        if (m.kind == EL_AM_MARK_PERSON || m.kind == EL_AM_MARK_BOSS) d *= 0.5f; // (over what they stand on)
        if (d < best_d) best_d = d, best = int(i);
    }
    return best;
}

// The label of the mark under the mouse, beside it (KF4's)
void draw_hover_label(ImDrawList *dl, ImVec2 q, const char *label, const float *g, float ts) {
    ImFont *font = ImGui::GetFont();
    const ImVec2 ext = font->CalcTextSizeA(ts, FLT_MAX, ts * 24.0f, label);
    const float pad = ts * 0.4f;
    ImVec2 at(q.x + ts, q.y + ts * 0.6f);
    if (at.x + ext.x + pad > g[2]) at.x = q.x - ts - ext.x - pad;
    if (at.y + ext.y + pad > g[3]) at.y = q.y - ext.y - pad * 2.0f;
    at.x = std::max(at.x, g[0] + pad);
    at.y = std::max(at.y, g[1] + pad);
    dl->AddRectFilled(ImVec2(at.x - pad, at.y - pad), ImVec2(at.x + ext.x + pad, at.y + ext.y + pad), IM_COL32(20, 17, 12, 235), pad);
    dl->AddRect(ImVec2(at.x - pad, at.y - pad), ImVec2(at.x + ext.x + pad, at.y + ext.y + pad), IM_COL32(200, 180, 130, 230), pad);
    dl->AddText(font, ts, at, IM_COL32(240, 230, 210, 255), label, nullptr, ts * 24.0f);
}

// ---- the tests' pictures (ELTALE_AUTOMAP_SHOTS, with the game's truth view) ------------------------

const bool shots = std::getenv("ELTALE_AUTOMAP_SHOTS") != nullptr;
uint32_t shot_latched; // flagged shots not made yet
int shot_map_id, shot_sub_id;
uint32_t shot_kind;    // the picture being made (EL_AM_PLAYER_SHOT_*), until the renderer wrote it
Sample shot_over_sample, shot_map_sample; // the ticks that asked for them (drawn as they were, however late)
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

// ---- fog of war ------------------------------------------------------------------------------------------
//
// What the player has seen, per sub-area, as cells of the reach grid (KF4's way: rays from the player stopped by
// walls; KF1's and KF3's maps fill in as you walk). Indoors a room that fits in 150 units of the minimap (a house,
// an inn, a castle room) is all seen as you enter it; a bigger indoor place (a hall, a cave, a dungeon floor) is seen
// 150 units around the player, and outdoors 400, the rays stopping at walls (a town's houses hide what is behind
// them). Unseen floor and walls are covered with the backdrop's colour (faint at the map's background opacity), and
// marks there are not drawn, except the cheats' (chests, spirits, bosses, story item givers, battle zones), as in
// KF3 and KF4. "Show the whole map" (a cheat) and the test pictures (ELTALE_AUTOMAP_SHOTS) turn it off.
//
// Kept per save, on the host only (the game's state is not touched): a save (the save menu's or F2's: the game's
// SaveMenu_CaptureGameState) writes automap/save_<key>.fog in the data folder, a load (SaveMenu_RestoreGameState:
// the save menu's, the title's Continue, F4) reads it back, the key being a hash of the saved record (nd_automap.c);
// the title (a reset, the end of the game, New Game) forgets it. A save made without this build has no file: its
// exploring starts from that point.

struct Seen {
    int nx = 0, nz = 0;
    float x0 = 0, z0 = 0, cs = 0;
    std::vector<uint8_t> cell; // 1: seen
};
std::map<uint32_t, Seen> seen_areas; // by map << 8 | sub-area
// what is drawn of the plan with fog of war (build_fog): only what has been seen, nothing over the rest (so the
// minimap's backdrop, where the game shows through, looks the same over unseen ground as beyond the area)
std::vector<uint8_t> fog_tri;        // per floor triangle: 0 unseen, 1 partly seen, 2 seen
struct FogCell {
    float x0, z0, x1, z1;
    uint32_t tri;
};
std::vector<FogCell> fog_cells;      // the seen cells of partly seen triangles (runs along x), drawn in their colour
std::vector<Edge> fog_walls, fog_edges, fog_wet; // the walls, the floor's edges and the water, cut to the seen cells
double fog_built_at = -1.0;
bool fog_dirty = true;
float fog_box[4];                    // the seen floor's bounds (the full map fits it)
bool fog_box_ok = false;
float rev_x = 1e30f, rev_z = 1e30f;  // where the last sight was taken
uint32_t rev_serial;
const bool fog_log = std::getenv("ELTALE_AUTOMAP_FOG_LOG") != nullptr;

bool fog_on() { return settings.map.fog && !settings.map.cheat_reveal && !shots; }

// the plan's seen cells (made, all unseen, when `make`); a record of another grid (a build that cuts the grid
// otherwise) counts as all seen
Seen *area_seen(bool make) {
    if (!plan.valid() || plan.gnx <= 0) return nullptr;
    const uint32_t k = uint32_t(plan.map) << 8 | uint32_t(plan.sub & 0xFF);
    auto it = seen_areas.find(k);
    const size_t n = size_t(plan.gnx) * size_t(plan.gnz);
    if (it == seen_areas.end()) {
        if (!make) return nullptr;
        Seen sn;
        sn.nx = plan.gnx; sn.nz = plan.gnz; sn.x0 = plan.gx0; sn.z0 = plan.gz0; sn.cs = plan.gcs;
        sn.cell.assign(n, 0);
        it = seen_areas.emplace(k, std::move(sn)).first;
    }
    Seen &sn = it->second;
    if (sn.nx != plan.gnx || sn.nz != plan.gnz || std::fabs(sn.x0 - plan.gx0) > 0.01f || std::fabs(sn.z0 - plan.gz0) > 0.01f ||
        std::fabs(sn.cs - plan.gcs) > 0.001f || sn.cell.size() != n) {
        sn.nx = plan.gnx; sn.nz = plan.gnz; sn.x0 = plan.gx0; sn.z0 = plan.gz0; sn.cs = plan.gcs;
        sn.cell.assign(n, 1);
    }
    return &sn;
}

size_t seen_count() {
    size_t c = 0;
    for (const auto &a : seen_areas)
        for (uint8_t v : a.second.cell) c += v;
    return c;
}

// What the player sees from (x, z): the whole of a small room, else rays out to the sight's reach, stopping at walls
void reveal(float x, float z) {
    if (!plan.reach_done || plan.gnx <= 0) return;
    Seen *sn = area_seen(true);
    if (!sn) return;
    const bool again = rev_serial == plan.serial;
    if (again && std::fabs(x - rev_x) < plan.gcs * 0.5f && std::fabs(z - rev_z) < plan.gcs * 0.5f) return;
    rev_serial = plan.serial; rev_x = x; rev_z = z;
    const bool indoor = !(plan.flags & EL_AM_AREA_SKY);
    const float *b = plan.reach_box;
    const float ext = std::max(b[2] - b[0], b[3] - b[1]) * 0.5f * 1.08f + 6.0f;
    size_t added = 0;
    if (indoor && ext <= 150.0f) { // a room: all of it
        for (uint8_t &c : sn->cell)
            if (!c) c = 1, added++;
    } else {
        const float reach = (indoor ? 150.0f : 400.0f) / plan.gcs;
        const float px = (x - plan.gx0) / plan.gcs, pz = (z - plan.gz0) / plan.gcs;
        const int rays = std::clamp(int(reach * 6.3f), 64, 1440);
        for (int r = 0; r < rays; r++) {
            const float a = float(r) * 6.2831853f / float(rays), dx = std::cos(a), dz = std::sin(a);
            for (float t = 0; t <= reach; t += 0.5f) {
                const int ix = int(std::floor(px + dx * t)), iz = int(std::floor(pz + dz * t));
                if (ix < 0 || iz < 0 || ix >= plan.gnx || iz >= plan.gnz) break;
                const size_t c = size_t(iz) * size_t(plan.gnx) + size_t(ix);
                if (!sn->cell[c]) sn->cell[c] = 1, added++;
                if (t > 1.5f && !plan.blocked.empty() && plan.blocked[c]) break; // (the wall itself is seen)
            }
        }
    }
    if (added) fog_dirty = true;
}

// What is drawn with fog of war: the triangles all seen, the seen cells of those partly seen, the walls, edges and
// water cut to the seen cells; and the seen floor's bounds. Cells count by their triangle (the grid's cell_tri); a
// triangle too small for a cell's centre goes by the cell under its middle.
void build_fog() {
    fog_dirty = false;
    fog_built_at = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
    fog_tri.clear(); fog_cells.clear(); fog_walls.clear(); fog_edges.clear(); fog_wet.clear();
    fog_box_ok = false;
    if (!plan.valid() || plan.gnx <= 0 || plan.cell_tri.empty()) return;
    const Seen *sn = area_seen(false);
    const int nx = plan.gnx, nz = plan.gnz;
    const float cs = plan.gcs;
    auto seen = [&](size_t c) { return sn && sn->cell[c] != 0; };
    auto seen_at = [&](float x, float z) {
        int ix, iz;
        return plan.cell_of(x, z, &ix, &iz) && seen(size_t(iz) * size_t(nx) + size_t(ix));
    };
    const size_t n = plan.tris.size();
    std::vector<uint32_t> total(n, 0), got(n, 0);
    float b[4] = { 1e30f, 1e30f, -1e30f, -1e30f };
    for (int kz = 0; kz < nz; kz++)
        for (int ix = 0; ix < nx; ix++) {
            const size_t c = size_t(kz) * size_t(nx) + size_t(ix);
            const bool sc = seen(c);
            if (sc && !plan.walk.empty() && plan.walk[c]) {
                const float x = plan.gx0 + float(ix) * cs, z = plan.gz0 + float(kz) * cs;
                b[0] = std::min(b[0], x); b[1] = std::min(b[1], z); b[2] = std::max(b[2], x + cs); b[3] = std::max(b[3], z + cs);
            }
            const int32_t t = plan.cell_tri[c];
            if (t >= 0 && size_t(t) < n) total[size_t(t)]++, got[size_t(t)] += sc;
        }
    fog_tri.assign(n, 0);
    for (size_t t = 0; t < n; t++) {
        if (!total[t]) {
            const ElAmTri &tr = plan.tris[t];
            fog_tri[t] = seen_at((tr.v[0][0] + tr.v[1][0] + tr.v[2][0]) / 3, (tr.v[0][2] + tr.v[1][2] + tr.v[2][2]) / 3) ? 2 : 0;
        } else {
            fog_tri[t] = !got[t] ? 0 : got[t] == total[t] ? 2 : 1;
        }
    }
    for (int kz = 0; kz < nz; kz++) {
        int run = -1, rt = -1;
        for (int ix = 0; ix <= nx; ix++) {
            int key = -1;
            if (ix < nx) {
                const size_t c = size_t(kz) * size_t(nx) + size_t(ix);
                const int32_t t = plan.cell_tri[c];
                if (t >= 0 && size_t(t) < n && fog_tri[size_t(t)] == 1 && seen(c)) key = t;
            }
            if (run >= 0 && key != rt) {
                fog_cells.push_back({ plan.gx0 + float(run) * cs, plan.gz0 + float(kz) * cs, plan.gx0 + float(ix) * cs,
                                      plan.gz0 + float(kz + 1) * cs, uint32_t(rt) });
                run = -1;
            }
            if (key >= 0 && run < 0) run = ix, rt = key;
        }
    }
    // a line cut into pieces of a cell, kept where a cell at or beside the piece's middle is seen (a ray that grazes a
    // wall sees only some of the cells it runs through)
    auto seen_near = [&](float x, float z) {
        int ix, iz;
        if (!plan.cell_of(x, z, &ix, &iz)) return false;
        for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++) {
                const int cx = ix + dx, cz = iz + dz;
                if (cx >= 0 && cz >= 0 && cx < nx && cz < nz && seen(size_t(cz) * size_t(nx) + size_t(cx))) return true;
            }
        return false;
    };
    auto cut = [&](float x0, float z0, float x1, float z1, std::vector<Edge> &out) {
        const float dx = x1 - x0, dz = z1 - z0, len = std::sqrt(dx * dx + dz * dz);
        const int steps = std::max(1, int(std::ceil(len / cs)));
        int start = -1;
        for (int k = 0; k <= steps; k++) {
            const bool on = k < steps && seen_near(x0 + dx * (float(k) + 0.5f) / float(steps), z0 + dz * (float(k) + 0.5f) / float(steps));
            if (on && start < 0) start = k;
            if (!on && start >= 0) {
                out.push_back({ x0 + dx * float(start) / float(steps), z0 + dz * float(start) / float(steps), x0 + dx * float(k) / float(steps),
                                z0 + dz * float(k) / float(steps) });
                start = -1;
            }
        }
    };
    for (const ElAmWall &w : plan.walls) cut(w.x0, w.z0, w.x1, w.z1, fog_walls);
    for (const Edge &e : plan.edges) cut(e.x0, e.z0, e.x1, e.z1, fog_edges);
    for (const Edge &r : plan.wet) { // (a run of cells along x)
        const float z = (r.z0 + r.z1) * 0.5f;
        float start = 1e30f;
        for (float x = r.x0; x < r.x1 + cs * 0.5f; x += cs) {
            const bool on = x < r.x1 - cs * 0.01f && seen_at(x + cs * 0.5f, z);
            if (on && start > 1e29f) start = x;
            if (!on && start < 1e29f) {
                fog_wet.push_back({ start, r.z0, std::min(x, r.x1), r.z1 });
                start = 1e30f;
            }
        }
    }
    if (b[0] <= b[2]) {
        std::memcpy(fog_box, b, sizeof b);
        fog_box_ok = true;
    }
}

// the seen cells of partly seen triangles, in their triangle's colour (its corners' mean)
void draw_fog_cells(ImDrawList *dl, const Xform &x, float py, float alpha) {
    for (const FogCell &fc : fog_cells) {
        if (fc.tri >= plan.tris.size()) continue;
        const ElAmTri &t = plan.tris[fc.tri];
        const bool r = fc.tri < plan.reach.size() ? plan.reach[fc.tri] != 0 : true;
        float sum[4] = {};
        for (int j = 0; j < 3; j++) {
            const ImU32 c = floor_color(t, j, plan.slope[fc.tri], py, alpha, r);
            for (int k = 0; k < 4; k++) sum[k] += float((c >> (8 * k)) & 0xFF);
        }
        const ImU32 col = IM_COL32(int(sum[0] / 3), int(sum[1] / 3), int(sum[2] / 3), int(sum[3] / 3));
        const ImVec2 a = x.at(fc.x0, fc.z0), b = x.at(fc.x1, fc.z1);
        dl->AddRectFilled(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), col);
    }
}

// a mark the player has seen the place of (or a cheat's, shown anyway)
bool discovered(const ElAmMarker &m) {
    if (!fog_on()) return true;
    if (m.kind == EL_AM_MARK_CHEST || m.kind == EL_AM_MARK_SPIRIT || m.kind == EL_AM_MARK_BOSS) return true;
    if (m.kind == EL_AM_MARK_PERSON && (m.flags & EL_AM_MARK_KEY_GIVER) && settings.map.cheat_givers) return true;
    const Seen *sn = area_seen(false);
    if (!sn) return false;
    int ix, iz;
    if (!plan.cell_of(m.x, m.z, &ix, &iz)) return false;
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            const int x = ix + dx, z = iz + dz;
            if (x >= 0 && z >= 0 && x < plan.gnx && z < plan.gnz && sn->cell[size_t(z) * size_t(plan.gnx) + size_t(x)]) return true;
        }
    return false;
}

// ---- its files: automap/save_<key>.fog in the data folder, every number little-endian:
//   "ELTFOG\0\0", u32 version (1), u32 areas, then per area: s32 map, s32 sub-area, s32 nx, s32 nz, f32 x0, z0, cell,
//   u32 runs, then the runs' lengths (u32 each, unseen and seen in turn, starting unseen), and last a u64 FNV-1a hash
//   of everything before it. Anything wrong (missing, short, another version, a bad hash): nothing seen.
std::string fog_path(uint64_t key) {
    char name[64];
    std::snprintf(name, sizeof name, "/automap/save_%016llx.fog", (unsigned long long)key);
    return data_dir() + name;
}

void put32(std::string &o, uint32_t v) { for (int i = 0; i < 4; i++) o += char((v >> (8 * i)) & 0xFF); }
uint32_t get32(const unsigned char *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t fnv64(const std::string &s, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 0x100000001B3ull;
    return h;
}

bool fog_write(uint64_t key) {
    std::string o("ELTFOG\0\0", 8);
    put32(o, 1);
    put32(o, uint32_t(seen_areas.size()));
    for (const auto &a : seen_areas) {
        const Seen &sn = a.second;
        put32(o, a.first >> 8); put32(o, a.first & 0xFF); put32(o, uint32_t(sn.nx)); put32(o, uint32_t(sn.nz));
        uint32_t f[3];
        std::memcpy(&f[0], &sn.x0, 4); std::memcpy(&f[1], &sn.z0, 4); std::memcpy(&f[2], &sn.cs, 4);
        for (uint32_t v : f) put32(o, v);
        std::vector<uint32_t> runs;
        uint8_t want = 0;
        uint32_t len = 0;
        for (uint8_t c : sn.cell) {
            if ((c != 0) != (want != 0)) { runs.push_back(len); len = 0; want ^= 1; }
            len++;
        }
        runs.push_back(len);
        put32(o, uint32_t(runs.size()));
        for (uint32_t r : runs) put32(o, r);
    }
    const uint64_t h = fnv64(o, o.size());
    put32(o, uint32_t(h)); put32(o, uint32_t(h >> 32));
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(data_dir() + "/automap"), ec);
    const std::string path = fog_path(key), tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(o.data(), 1, o.size(), f) == o.size();
    std::fclose(f);
    if (!ok) { std::remove(tmp.c_str()); return false; }
    std::filesystem::rename(std::filesystem::u8path(tmp), std::filesystem::u8path(path), ec);
    return !ec;
}

// 1 read, 0 no file, -1 a bad file (nothing taken)
int fog_read(uint64_t key) {
    FILE *f = std::fopen(fog_path(key).c_str(), "rb");
    if (!f) return 0;
    std::string d;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) d.append(buf, n);
    std::fclose(f);
    const unsigned char *p = (const unsigned char *)d.data();
    if (d.size() < 24 || d.compare(0, 8, std::string("ELTFOG\0\0", 8)) || get32(p + 8) != 1) return -1;
    const uint64_t h = uint64_t(get32(p + d.size() - 8)) | uint64_t(get32(p + d.size() - 4)) << 32;
    if (h != fnv64(d, d.size() - 8)) return -1;
    std::map<uint32_t, Seen> got;
    size_t at = 16;
    const size_t end = d.size() - 8;
    for (uint32_t i = get32(p + 12); i; i--) {
        if (at + 32 > end) return -1;
        Seen sn;
        const uint32_t map = get32(p + at), sub = get32(p + at + 4);
        sn.nx = int(get32(p + at + 8)); sn.nz = int(get32(p + at + 12));
        uint32_t fv[3] = { get32(p + at + 16), get32(p + at + 20), get32(p + at + 24) };
        std::memcpy(&sn.x0, &fv[0], 4); std::memcpy(&sn.z0, &fv[1], 4); std::memcpy(&sn.cs, &fv[2], 4);
        const uint32_t runs = get32(p + at + 28);
        at += 32;
        if (sn.nx <= 0 || sn.nz <= 0 || size_t(sn.nx) * size_t(sn.nz) > (size_t(1) << 26) || map > 255 || sub > 255 ||
            at + size_t(runs) * 4 > end)
            return -1;
        sn.cell.reserve(size_t(sn.nx) * size_t(sn.nz));
        for (uint32_t r = 0; r < runs; r++, at += 4) sn.cell.insert(sn.cell.end(), get32(p + at), uint8_t(r & 1));
        if (sn.cell.size() != size_t(sn.nx) * size_t(sn.nz)) return -1;
        got[map << 8 | sub] = std::move(sn);
    }
    seen_areas = std::move(got);
    return 1;
}

bool in_play() { return have_sample && plan.valid() && (newest.flags & EL_AM_PLAYER_PLAY) && newest.area == plan.serial; }

void fit(const float *area) {
    // the plan's bounds, with a margin of 4%; small rooms are not blown up past a sixth of the picture's height a
    // hundred units
    const float *b = fog_on() && fog_box_ok ? fog_box : plan.reach_box; // (the seen floor, with fog of war)
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
// the mark under the mouse (its id, 0 none); ELTALE_AUTOMAP_HOVER=person|door|exit|chest|spirit|boss (a test's
// pictures): the mouse put on the first mark of that kind
uint32_t hovered_id;
const uint16_t hover_test_kind = [] {
    const char *e = std::getenv("ELTALE_AUTOMAP_HOVER");
    if (!e) return uint16_t(0);
    const char *names[] = { "", "door", "exit", "person", "chest", "spirit", "boss", "sign" };
    for (uint16_t k = 1; k < 8; k++)
        if (!std::strcmp(e, names[k])) return k;
    return uint16_t(0);
}();
// where the minimap was last drawn (the wheel over it zooms it), and the zoom's note until when (ImGui's time)
float mm_rect[4];
bool mm_shown;
double zoom_note_until;

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
    const Sample p = shots && (shot_kind & EL_AM_PLAYER_SHOT_MAP) ? shot_map_sample : player_now(!shots && !dump_test);
    if (dump_test && view_changes != view_dumped) {
        view_dumped = view_changes;
        char name[32];
        std::snprintf(name, sizeof name, "automap_full_%d", view_changes);
        renderer()->request_dump(name);
        std::printf("P1_AUTOMAP_VIEW n=%d map=%d sub=%d zoom=%.3f centre=%.1f,%.1f seen=%zu\n", view_changes, plan.map, plan.sub,
                    view_zoom, view_cx, view_cz, seen_count());
        std::fflush(stdout);
    }
    dl->PushClipRect(ImVec2(area[0], area[1]), ImVec2(area[2], area[3]), true);
    draw_floor(dl, x, p.y, 1.0f);
    draw_wet(dl, x, 1.0f);
    const float wall_w = std::clamp(x.s * 3.0f, 1.0f, gh / 220.0f), edge_w = std::clamp(x.s * 1.5f, 1.0f, gh / 400.0f);
    draw_lines(dl, x, wall_w, edge_w, 1.0f);
    const bool zones = zones_on();
    { // (held or let go: a test's picture and line)
        static bool was;
        const bool now = zones_held();
        if (now != was) {
            was = now;
            view_changes++;
            if (dump_test) {
                std::printf("P1_AUTOMAP_ZONES held=%d by=%s zones=%zu runs=%zu button=%.0f,%.0f,%.0f,%.0f\n", int(now),
                            !now ? "none" : zones_mouse ? "button" : "key", plan.zones.size(), plan.zone_runs.size(), zb_rect[0], zb_rect[1], zb_rect[2],
                            zb_rect[3]);
                std::fflush(stdout);
            }
        }
    }
    if (zones) draw_zones(dl, x, 1.0f, std::max(1.5f, gh / 400.0f));
    const std::vector<ElAmMarker> marks = shots && (shot_kind & EL_AM_PLAYER_SHOT_MAP) ? shot_map_sample.marks : marks_now(!shots && !dump_test);
    const float mr = std::clamp(x.s * 5.0f, gh / 110.0f, gh / 55.0f);
    draw_marks(dl, x, marks, mr, 1.0f, false, area);
    if (p.area == plan.serial)
        draw_arrow(dl, x.at(p.x, p.z), p.rot, std::max(9.0f, std::min(gh / 45.0f, x.s * 20.0f)), 1.0f);
    // the mark under the mouse, named (or, for a test's picture, ELTALE_AUTOMAP_HOVER's kind: the first such mark)
    {
        ImVec2 q(mouse_x, mouse_y);
        bool over = inside && !dragging;
        if (hover_test_kind) {
            over = false;
            for (const ElAmMarker &m : marks)
                if (m.kind == hover_test_kind && mark_shown(m, false)) { q = x.at(m.x, m.z); over = true; break; }
        }
        const int h = over ? pick_mark(x, marks, q, std::max(mr * 1.6f, 10.0f), false) : -1;
        if (h >= 0) {
            hovered_id = marks[size_t(h)].id;
            draw_hover_label(dl, q, hover_text(marks[size_t(h)], false).c_str(), area, ts);
        } else {
            hovered_id = 0;
            // over a battle zone (shown): its name and monsters
            const bool zone_test = std::getenv("ELTALE_AUTOMAP_HOVER") && !std::strcmp(std::getenv("ELTALE_AUTOMAP_HOVER"), "zone");
            if (zones && zone_test && !plan.zone_runs.empty()) {
                const Edge &r = plan.zone_runs[plan.zone_runs.size() / 2];
                q = x.at((r.x0 + r.x1) * 0.5f, (r.z0 + r.z1) * 0.5f);
                over = true;
            }
            const int z = zones && over ? plan.zone_at(x.wx(q.x), x.wz(q.y)) : 0;
            if (z) draw_hover_label(dl, q, zone_label(z).c_str(), area, ts);
        }
    }
    // the battle zones' button (the cheat on, the area has zones): held, the zones are shown
    zb_shown = false;
    if (settings.map.cheat_zones && !plan.zone_runs.empty()) {
        const char *bl = "Battle zones (hold)";
        const ImVec2 ext = ImGui::GetFont()->CalcTextSizeA(ts, FLT_MAX, 0.0f, bl);
        const float bw = ext.x + ts * 1.6f, bh = ts * 1.7f;
        const float bx = (area[0] + area[2]) * 0.5f - bw * 0.5f, by = area[3] - bh - gh * 0.012f;
        zb_rect[0] = bx; zb_rect[1] = by; zb_rect[2] = bx + bw; zb_rect[3] = by + bh;
        zb_shown = true;
        const bool hot = zones_held() || zones_test;
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), hot ? IM_COL32(150, 35, 30, 240) : IM_COL32(40, 30, 26, 225), bh * 0.25f);
        dl->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh), hot ? IM_COL32(255, 120, 100, 255) : IM_COL32(200, 120, 100, 230), bh * 0.25f,
                    0, 1.5f);
        draw_text(dl, ts, ImVec2(bx + ts * 0.8f, by + (bh - ext.y) * 0.5f), IM_COL32(245, 225, 210, 255), bl);
    }
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
    // the marks this area has, as they are drawn (an icon in the swatch's place)
    auto has = [&](uint16_t kind, uint16_t flags, uint16_t no) {
        for (const ElAmMarker &m : marks)
            if (m.kind == kind && (m.flags & flags) == flags && !(m.flags & no) && mark_shown(m, false)) return true;
        return false;
    };
    auto icon = [&](uint16_t kind, uint16_t flags, float angle, const char *label) {
        ElAmMarker m{};
        m.kind = kind;
        m.flags = flags;
        m.angle = angle;
        const Xform lxf{ lx + sw * 0.5f, ly + ts * 0.55f, 0.0f, 0.0f, 1.0f };
        m.half_w = sw * 0.45f;
        m.half_d = ts * 0.3f;
        draw_mark(dl, lxf, m, ts * 0.45f, 1.0f, true);
        draw_text(dl, ts, ImVec2(lx + sw * 1.4f, ly), COL_HELP, label);
        ly += ts * 1.5f;
    };
    if (has(EL_AM_MARK_DOOR, 0, EL_AM_MARK_AWAY | EL_AM_MARK_LOCKED)) icon(EL_AM_MARK_DOOR, 0, 0.0f, "Door");
    if (has(EL_AM_MARK_DOOR, EL_AM_MARK_AWAY, EL_AM_MARK_LOCKED)) icon(EL_AM_MARK_DOOR, EL_AM_MARK_AWAY, 0.0f, "Door to another area");
    if (has(EL_AM_MARK_DOOR, EL_AM_MARK_LOCKED, 0) || has(EL_AM_MARK_EXIT, EL_AM_MARK_LOCKED, 0))
        icon(EL_AM_MARK_DOOR, EL_AM_MARK_LOCKED, 0.0f, "Locked");
    if (has(EL_AM_MARK_EXIT, 0, EL_AM_MARK_AWAY | EL_AM_MARK_LOCKED)) icon(EL_AM_MARK_EXIT, 0, 0.0f, "Way through");
    if (has(EL_AM_MARK_EXIT, EL_AM_MARK_AWAY, EL_AM_MARK_LOCKED)) icon(EL_AM_MARK_EXIT, EL_AM_MARK_AWAY, 0.0f, "Way to another area");
    if (has(EL_AM_MARK_PERSON, 0, 0)) icon(EL_AM_MARK_PERSON, 0, 0.0f, "Person");
    {
        bool story = false, giver = false;
        unsigned svc = 0;
        for (const ElAmMarker &m : marks) {
            if (!mark_shown(m, false)) continue;
            if (m.kind == EL_AM_MARK_PERSON) {
                story |= story_shown(m, false);
                giver |= giver_shown(m, false);
                if (m.service && service_shown(false)) svc |= EL_AM_SVC_BIT(m.service);
            }
            svc |= door_services(m, false);
        }
        if (story) icon(EL_AM_MARK_PERSON, EL_AM_MARK_STORY, 0.0f, "Has a story line");
        if (giver) icon(EL_AM_MARK_PERSON, EL_AM_MARK_KEY_GIVER, 0.0f, "Gives a story item");
        for (int k = 1; k < EL_AM_SVC_COUNT; k++)
            if (svc & EL_AM_SVC_BIT(k)) {
                draw_service_icon(dl, ImVec2(lx + sw * 0.5f, ly + ts * 0.55f), ts * 0.5f, k, 1.0f);
                draw_text(dl, ts, ImVec2(lx + sw * 1.4f, ly), COL_HELP, SVC_NAME[k]);
                ly += ts * 1.5f;
            }
    }
    if (has(EL_AM_MARK_SIGN, 0, 0)) icon(EL_AM_MARK_SIGN, 0, 0.0f, "Sign");
    if (zones) swatch(fade(COL_ZONE, 0.55f), "Battle zone");
    if (has(EL_AM_MARK_CHEST, 0, EL_AM_MARK_DONE)) icon(EL_AM_MARK_CHEST, 0, 0.0f, "Chest");
    if (has(EL_AM_MARK_CHEST, EL_AM_MARK_DONE, 0)) icon(EL_AM_MARK_CHEST, EL_AM_MARK_DONE, 0.0f, "Chest (opened)");
    if (has(EL_AM_MARK_SPIRIT, 0, EL_AM_MARK_DONE)) icon(EL_AM_MARK_SPIRIT, 0, 0.0f, "Spirit");
    if (has(EL_AM_MARK_SPIRIT, EL_AM_MARK_DONE, 0)) icon(EL_AM_MARK_SPIRIT, EL_AM_MARK_DONE, 0.0f, "Spirit (taken)");
    if (has(EL_AM_MARK_BOSS, 0, 0)) icon(EL_AM_MARK_BOSS, 0, 0.0f, "Boss");
    draw_arrow(dl, ImVec2(lx + sw * 0.5f, ly + ts * 0.55f), 0.0f, ts * 0.55f, 1.0f);
    draw_text(dl, ts, ImVec2(lx + sw * 1.4f, ly), COL_HELP, "You");
    ly += ts * 2.2f;
    std::string key = "M";
    for (int s = 0; s < BIND_SLOTS - 1; s++)
        if (settings.controls.bind[ACT_MAP][s]) { key = binding_label(settings.controls.bind[ACT_MAP][s]); break; }
    std::string help = "Wheel: zoom\nDrag: move\nRight click: fit\nPoint at a mark: what it is\n" + key + " or Esc: close";
    if (zb_shown) {
        std::string zk;
        for (int s = 0; s < BIND_SLOTS; s++)
            if (settings.controls.bind[ACT_MAP_ZONES][s]) { zk = binding_label(settings.controls.bind[ACT_MAP_ZONES][s]); break; }
        if (!zk.empty()) help += "\nHold " + zk + ": battle zones";
    }
    draw_text(dl, ts * 0.85f, ImVec2(lx, ly), COL_HELP, help.c_str());
    // north
    draw_text(dl, ts * 1.2f, ImVec2(area[2] - ts * 1.6f, area[1]), COL_NORTH, "N");
}

// The minimap's view: outdoors (an area with a sky) its outdoor reach, indoors its own (rooms are small: a house's
// is 60-100 units across); a room whose reachable floor fits in the reach fills the minimap, centred on the room, so
// you see the whole room and its doors and people (Map page: "Fit small rooms").
struct MinimapView {
    bool indoor = false, fitted = false;
    float reach = 800.0f, cx = 0, cz = 0;
};
MinimapView minimap_view() {
    MinimapView v;
    const MapSettings &m = settings.map;
    v.indoor = plan.valid() && !(plan.flags & EL_AM_AREA_SKY);
    v.reach = float(v.indoor ? m.reach_in : m.reach);
    if (m.fit_rooms && v.indoor && plan.reach_done) {
        const float *b = plan.reach_box;
        const float ext = std::max(b[2] - b[0], b[3] - b[1]) * 0.5f * 1.08f + 6.0f;
        if (ext <= v.reach) {
            v.reach = std::max(ext, 20.0f);
            v.fitted = true;
            v.cx = (b[0] + b[2]) * 0.5f;
            v.cz = (b[1] + b[3]) * 0.5f;
        }
    }
    return v;
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
    // (the backdrop: 75% at the default 85% opacity, solid at 100%)
    const float back = a <= 0.85f ? 0.75f * a / 0.85f : 0.75f + (a - 0.85f) / 0.15f * 0.25f;
    dl->AddRectFilled(p0, p1, rgba(10, 9, 8, 255 * back), round);
    const Sample p = player_now(!dump_test);
    // (ELTALE_AUTOMAP_MINIMAP_AT=M,S: in that sub-area only)
    static int at_map = -2, at_sub = -1;
    if (at_map == -2) {
        const char *e = std::getenv("ELTALE_AUTOMAP_MINIMAP_AT");
        if (!e || std::sscanf(e, "%d,%d", &at_map, &at_sub) != 2) at_map = -1;
    }
    if (dump_test && !minimap_dumped && samples_on_plan >= (at_map < 0 ? 100u : 250u) && (at_map < 0 || (plan.map == at_map && plan.sub == at_sub))) {
        minimap_dumped = true;
        renderer()->request_dump("automap_minimap");
        const MinimapView v = minimap_view();
        std::printf("P1_AUTOMAP_MINIMAP map=%d sub=%d rect=%.0f,%.0f,%.0f,%.0f indoor=%d fitted=%d reach=%.1f\n", plan.map, plan.sub,
                    x0, y0, x0 + side, y0 + side, v.indoor, v.fitted, v.reach);
        std::fflush(stdout);
    }
    float cx = p.x, cz = p.z;
    const MinimapView mv = minimap_view();
    if (mv.fitted) cx = mv.cx, cz = mv.cz;
    Xform x{ x0 + side * 0.5f, y0 + side * 0.5f, cx, cz, side * 0.5f / mv.reach };
    mm_rect[0] = p0.x; mm_rect[1] = p0.y; mm_rect[2] = p1.x; mm_rect[3] = p1.y;
    mm_shown = true;
    const float clip[4] = { p0.x + 1, p0.y + 1, p1.x - 1, p1.y - 1 };
    dl->PushClipRect(ImVec2(clip[0], clip[1]), ImVec2(clip[2], clip[3]), true);
    draw_floor(dl, x, p.y, a);
    draw_wet(dl, x, a);
    draw_lines(dl, x, std::clamp(x.s * 3.0f, 1.0f, side / 90.0f), std::clamp(x.s * 1.5f, 1.0f, side / 160.0f), a);
    if (zones_on()) draw_zones(dl, x, a, 1.5f);
    draw_marks(dl, x, marks_now(!dump_test), std::clamp(x.s * 5.0f, side / 42.0f, side / 20.0f), a, false, clip);
    draw_arrow(dl, x.at(p.x, p.z), p.rot, std::max(5.0f, side * 0.045f), a);
    dl->PopClipRect();
    // after a zoom key or the wheel: the reach for a moment
    if (zoom_note_until > 0 && ImGui::GetTime() < zoom_note_until) {
        char note[64];
        std::snprintf(note, sizeof note, mv.fitted ? "%s: the whole room" : "%s: %d units", mv.indoor ? "Indoors" : "Outdoors",
                      int(mv.reach + 0.5f));
        const float ns = std::max(11.0f, side * 0.065f);
        const ImVec2 ext = ImGui::GetFont()->CalcTextSizeA(ns, FLT_MAX, 0.0f, note);
        const ImVec2 at(x0 + (side - ext.x) * 0.5f, y0 + side - ext.y - side * 0.03f);
        dl->AddRectFilled(ImVec2(at.x - 3, at.y - 1), ImVec2(at.x + ext.x + 3, at.y + ext.y + 1), rgba(10, 9, 8, 200 * a), 3.0f);
        draw_text(dl, ns, at, (COL_TITLE & 0x00FFFFFFu) | (ImU32(255 * a) << 24), note);
    }
    dl->AddRect(p0, p1, rgba(200, 180, 130, 255 * 0.8f * a), round, 0, 1.5f);
    const float ns = std::max(12.0f, side * 0.08f);
    draw_text(dl, ns, ImVec2(x0 + side * 0.5f - ns * 0.3f, y0 + 2), (COL_NORTH & 0x00FFFFFFu) | (ImU32(255 * a) << 24), "N");
}

std::string json_text(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '\n') { out += "\\n"; continue; }
        if (c == '"' || c == '\\') out += '\\';
        if ((unsigned char)c >= 0x20) out += c;
    }
    return out;
}

// ---- the danger meter (a cheat): the chance the next 50 units walked start a random battle --------------------
//
// Battle_TryStartRandomEncounter: every 50 units walked (battle_travel_accumulator) the limit grows by 50
// (battle_encounter_roll_limit) and a battle starts when Math_Rand(2000) < limit, so the next roll's chance is
// (limit + 50) / 2000, capped at 100%; it starts only where the player is in a region with an encounter point 50..90
// units away (EL_AM_DANGER_ZONE), elsewhere the roll is made and lost (the limit keeps growing). Read-only.
//
// It sits against the minimap (above it in a bottom corner, below it in a top one), the width of the minimap: the
// eye is already there while walking, and the full map hides the game anyway. With the minimap off it keeps the
// minimap's corner.

float danger_shown = -1.0f; // the chance drawn (eased toward the game's at any frame rate)

void draw_danger(ImDrawList *dl, const float *g) {
    const float gh = g[3] - g[1];
    const MapSettings &m = settings.map;
    const float side = gh * m.size / 100.0f, margin = gh * 0.025f;
    const bool right = m.corner == 0 || m.corner == 1, bottom = m.corner == 1 || m.corner == 3;
    const float mx0 = right ? g[2] - margin - side : g[0] + margin;
    const float my0 = bottom ? g[3] - margin - side : g[1] + gh * (right ? 0.27f : 0.24f);
    const float h = std::max(16.0f, gh * 0.034f), gap = gh * 0.008f;
    float y0;
    if (m.minimap) y0 = bottom ? my0 - gap - h : my0 + side + gap;
    else y0 = bottom ? g[3] - margin - h : my0;
    const float x0 = mx0, x1 = mx0 + side, y1 = y0 + h;
    const float a = std::max(0.6f, m.opacity / 100.0f);

    // the samples the picture is between: the walk toward the next roll slides with the picture (a roll wraps it)
    float travel = newest.travel;
    uint16_t limit = newest.roll_limit, danger = newest.danger;
    uint32_t flags = newest.flags;
    const render::NativeDraw::Shown &sh = render::native_draw().shown_frames();
    if (!dump_test && sh.valid) {
        const Sample *sa = find_sample(sh.a), *sb = find_sample(sh.b);
        if (sa && sb && sa != sb && sa->area == sb->area && sh.t > 0) {
            float tb = sb->travel;
            if (tb < sa->travel) tb += 50.0f;
            travel = sa->travel + (tb - sa->travel) * sh.t;
            if (travel >= 50.0f) travel -= 50.0f;
            limit = sb->roll_limit; danger = sb->danger; flags = sb->flags;
        } else if (sa) {
            travel = sa->travel; limit = sa->roll_limit; danger = sa->danger; flags = sa->flags;
        }
    }
    const float chance = std::min(1.0f, float(limit + 50) / 2000.0f);
    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.25f);
    if (danger_shown < 0 || dump_test) danger_shown = chance;
    else danger_shown += (chance - danger_shown) * (1.0f - std::exp(-dt / 0.12f));
    const bool battle = (flags & EL_AM_PLAYER_BATTLE) != 0, here = (danger & EL_AM_DANGER_ZONE) != 0;

    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), rgba(10, 9, 8, 215 * a), h * 0.25f);
    // the colour of the chance: green, amber, red
    const float c = danger_shown;
    const ImU32 col = here ? rgba(90 + 165 * std::min(1.0f, c * 4.0f), 200 - 150 * std::max(0.0f, c * 2.0f - 0.2f), 60, 255 * a)
                           : rgba(120, 125, 120, 200 * a);
    // the chance as a bar, the walk to the next roll as a thin strip under it
    const float pad = h * 0.14f, bx0 = x0 + pad, bx1 = x1 - pad;
    const float strip = std::max(2.0f, h * 0.14f);
    if (!battle) {
        dl->AddRectFilled(ImVec2(bx0, y0 + pad), ImVec2(bx0 + (bx1 - bx0) * std::clamp(c, 0.0f, 1.0f), y1 - pad - strip - 1),
                          fade(col, here ? 0.85f : 0.45f), h * 0.12f);
        dl->AddRectFilled(ImVec2(bx0, y1 - pad - strip), ImVec2(bx0 + (bx1 - bx0) * std::clamp(travel / 50.0f, 0.0f, 1.0f), y1 - pad),
                          rgba(235, 215, 160, (here ? 230 : 120) * a));
    }
    char text[64];
    if (battle) std::snprintf(text, sizeof text, "In battle");
    else if (here) std::snprintf(text, sizeof text, "Battle %d%% next step", int(std::lround(c * 100.0f)));
    else if (danger & EL_AM_DANGER_ROLLS) std::snprintf(text, sizeof text, "Safe here (%d%% in a zone)", int(std::lround(c * 100.0f)));
    else std::snprintf(text, sizeof text, "Safe here");
    const float ts = std::max(11.0f, h * 0.62f);
    const ImVec2 ext = ImGui::GetFont()->CalcTextSizeA(ts, FLT_MAX, 0.0f, text);
    const ImVec2 at(x0 + (side - ext.x) * 0.5f, y0 + (h - strip - ext.y) * 0.5f);
    dl->AddText(ImGui::GetFont(), ts, ImVec2(at.x + 1, at.y + 1), rgba(0, 0, 0, 220 * a), text);
    dl->AddText(ImGui::GetFont(), ts, at, rgba(245, 235, 215, 255 * a), text);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), rgba(200, 180, 130, 200 * a), h * 0.25f, 0, 1.0f);
    static int dumped;
    if (dump_test && !dumped && samples_on_plan >= 120) {
        dumped = 1;
        std::printf("P1_AUTOMAP_DANGER rect=%.0f,%.0f,%.0f,%.0f travel=%.2f limit=%u danger=%u text=%s\n", x0, y0, x1, y1, travel, limit,
                    danger, text);
        std::fflush(stdout);
    }
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
    // the marks of the over picture's tick (tools/automap_sheet.py's close-ups)
    std::fprintf(f, "],\n \"marks\": [");
    for (size_t i = 0; i < shot_over_sample.marks.size(); i++) {
        const ElAmMarker &m = shot_over_sample.marks[i];
        std::fprintf(f, "%s\n  {\"id\": %u, \"kind\": %u, \"flags\": %u, \"x\": %.2f, \"y\": %.2f, \"z\": %.2f, \"angle\": %.4f, "
                     "\"half_w\": %.2f, \"half_d\": %.2f, \"radius\": %.2f, \"item\": %u, \"to\": [%d, %d], \"service\": %u, "
                     "\"services\": %u, \"label\": \"%s\"}",
                     i ? "," : "", m.id, m.kind, m.flags, m.x, m.y, m.z, m.angle, m.half_w, m.half_d, m.radius, m.item, m.to_map,
                     m.to_sub, m.service, m.services, json_text(hover_text(m, true)).c_str());
    }
    std::fprintf(f, "],\n \"zones\": [");
    for (size_t i = 0; i < plan.zones.size(); i++) {
        const ElAmZone &z = plan.zones[i];
        size_t cells = 0;
        for (uint8_t c : plan.zone_cell) cells += c == i + 1;
        std::fprintf(f, "%s\n  {\"box\": [%.1f, %.1f, %.1f, %.1f], \"cells\": %zu, \"label\": \"%s\"}", i ? "," : "", z.x0, z.z0, z.x1,
                     z.z1, cells, json_text(z.label).c_str());
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
    draw_zones(dl, x, 1.0f, std::max(1.5f, gh / 500.0f));
    draw_lines(dl, x, std::max(1.5f, gh / 400.0f), std::max(1.0f, gh / 700.0f), 1.0f, IM_COL32(255, 40, 255, 255),
               IM_COL32(255, 230, 0, 255));
    const Sample p = shot_over_sample;
    // every mark (cheats too), named, over the real models
    const float r = std::max(7.0f, gh / 110.0f);
    draw_marks(dl, x, p.marks, r, 0.9f, true, nullptr);
    const float ts = std::max(11.0f, gh / 88.0f);
    for (const ElAmMarker &m : p.marks) {
        const ImVec2 q = x.at(m.x, m.z);
        const std::string t = hover_text(m, true);
        const ImVec2 ext = ImGui::GetFont()->CalcTextSizeA(ts, FLT_MAX, 0.0f, t.c_str());
        const ImVec2 at(q.x + r * 1.2f, q.y - ext.y * 0.5f);
        dl->AddRectFilled(ImVec2(at.x - 2, at.y), ImVec2(at.x + ext.x + 2, at.y + ext.y), IM_COL32(0, 0, 0, 150), 2.0f);
        draw_text(dl, ts, at, IM_COL32(255, 255, 200, 255), t.c_str());
    }
    // each zone named at its middle run
    for (size_t i = 0; i < plan.zones.size(); i++) {
        const Edge *mid = nullptr;
        size_t n = 0;
        for (const Edge &e : plan.zone_runs)
            if (plan.zone_at((e.x0 + e.x1) * 0.5f, (e.z0 + e.z1) * 0.5f) == int(i + 1)) n++;
        size_t k = 0;
        for (const Edge &e : plan.zone_runs)
            if (plan.zone_at((e.x0 + e.x1) * 0.5f, (e.z0 + e.z1) * 0.5f) == int(i + 1) && k++ == n / 2) { mid = &e; break; }
        if (!mid) continue;
        const ImVec2 q = x.at((mid->x0 + mid->x1) * 0.5f, (mid->z0 + mid->z1) * 0.5f);
        char t[24];
        std::snprintf(t, sizeof t, "zone %zu", i + 1);
        dl->AddRectFilled(ImVec2(q.x - 2, q.y), ImVec2(q.x + ts * 3.2f, q.y + ts), IM_COL32(60, 0, 0, 170), 2.0f);
        draw_text(dl, ts, q, IM_COL32(255, 200, 190, 255), t);
    }
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
    plan.water.assign(a->water, a->water + a->water_count);
    plan.zones.assign(a->zones, a->zones + a->zone_count);
    plan.points.clear();
    for (uint32_t i = 0; i < a->point_count; i++) plan.points.push_back({ a->points[i * 2], a->points[i * 2 + 1] });
    plan.walk.clear();
    plan.blocked.clear();
    plan.cell_tri.clear();
    plan.zone_cell.clear();
    plan.zone_runs.clear();
    plan.zone_edges.clear();
    plan.gnx = plan.gnz = 0;
    prepare(plan);
    fog_dirty = true;
    fitted = false; // (a new area: the open map fits it)
}

void receive_player(const ElAmPlayer *p) {
    if (!p) return;
    Sample s;
    s.frame = p->frame;
    s.area = p->area_serial;
    s.flags = p->flags;
    s.x = p->x; s.y = p->y; s.z = p->z; s.rot = p->rot;
    if (p->markers && p->marker_count) s.marks.assign(p->markers, p->markers + p->marker_count);
    s.travel = p->travel;
    s.roll_limit = p->roll_limit;
    s.danger = p->danger;
    s.zone = p->zone;
    // ELTALE_AUTOMAP_BATTLES=1 (a test): a random battle started; where the roll found the player (the last tick's
    // place, before the battle moved anyone), against the shaded zones and the rule at that exact point
    static const bool battles = std::getenv("ELTALE_AUTOMAP_BATTLES") != nullptr;
    if (battles && (p->flags & EL_AM_PLAYER_RANDOM_BATTLE) && have_sample && !(newest.flags & EL_AM_PLAYER_BATTLE) &&
        newest.area == plan.serial && plan.valid()) {
        if (!plan.reach_done) ensure_reach();
        const int shaded = plan.zone_at(newest.x, newest.z), rule = zone_rule(plan, newest.x, newest.z);
        int near = 0; // a shaded cell within one cell (the cells' centres stand for the cell)
        for (int dz = -1; dz <= 1 && !near; dz++)
            for (int dx = -1; dx <= 1 && !near; dx++) near = plan.zone_at(newest.x + float(dx) * plan.gcs, newest.z + float(dz) * plan.gcs);
        std::printf("P1_AUTOMAP_BATTLE_CHECK map=%d sub=%d x=%.3f z=%.3f shaded=%d near=%d rule=%d reach=%d %s\n", plan.map, plan.sub,
                    newest.x, newest.z, shaded, near, rule, [&] {
                        int ix, iz;
                        return plan.cell_of(newest.x, newest.z, &ix, &iz) && !plan.walk.empty() ? int(plan.walk[size_t(iz) * size_t(plan.gnx) + size_t(ix)]) : -1;
                    }(), shaded ? "INSIDE" : near ? "EDGE" : "OUTSIDE");
        std::fflush(stdout);
    }
    samples[sample_next] = s;
    sample_next = (sample_next + 1) % SAMPLES;
    newest = s;
    // fog of war: what the player sees from here (host only)
    if ((s.flags & EL_AM_PLAYER_PLAY) && plan.valid() && s.area == plan.serial) {
        have_sample = true;
        ensure_reach();
        const bool first = rev_serial != plan.serial;
        reveal(s.x, s.z);
        if (first && fog_dirty) build_fog();
    }
    if (p->flags & (EL_AM_PLAYER_SHOT_TRUTH | EL_AM_PLAYER_SHOT_OVER | EL_AM_PLAYER_SHOT_MAP)) {
        shot_latched |= p->flags & (EL_AM_PLAYER_SHOT_TRUTH | EL_AM_PLAYER_SHOT_OVER | EL_AM_PLAYER_SHOT_MAP);
        if (p->flags & EL_AM_PLAYER_SHOT_OVER) shot_over_sample = s;
        if (p->flags & EL_AM_PLAYER_SHOT_MAP) shot_map_sample = s;
        shot_map_id = plan.map; // (named for the area it was asked in)
        shot_sub_id = plan.sub;
    }
    have_sample = true;
    samples_on_plan = p->area_serial == plan.serial ? samples_on_plan + 1 : 0;
    // ELTALE_AUTOMAP_OPEN_AT=N (a test, with ELTALE_AUTOMAP_DUMP): the full map opens N ticks into each area (its
    // picture: what has been explored by then)
    static const int open_at = std::getenv("ELTALE_AUTOMAP_OPEN_AT") ? std::atoi(std::getenv("ELTALE_AUTOMAP_OPEN_AT")) : 0;
    if (open_at > 0 && samples_on_plan == uint32_t(open_at) && !full && in_play()) {
        full = true;
        fitted = false;
        view_changes++;
    }
}

// The game saved, loaded or went back to the title (nd_automap.c): the fog of war's files
void receive_save(int kind, uint64_t key) {
    const char *what = kind == EL_AM_SAVE_WRITE ? "write" : kind == EL_AM_SAVE_READ ? "read" : "title";
    int result = 1;
    if (kind == EL_AM_SAVE_WRITE) {
        result = fog_write(key) ? 1 : -1;
    } else if (kind == EL_AM_SAVE_READ) {
        seen_areas.clear();
        result = fog_read(key); // (no file: a save from before, exploring starts here; a bad one: the same)
        if (result < 0) seen_areas.clear();
    } else {
        seen_areas.clear(); // (the title: a reset, the game's end, before New Game or a load)
    }
    fog_dirty = true;
    rev_serial = 0;
    if (fog_log || dump_test) {
        std::printf("P1_AUTOMAP_FOG event=%s key=%016llx result=%d areas=%zu cells=%zu\n", what, (unsigned long long)key, result,
                    seen_areas.size(), seen_count());
        std::fflush(stdout);
    }
}

// ---- the host's side -----------------------------------------------------------------------------

void toggle() {
    view_changes++;
    if (full) { full = false; dragging = false; zones_mouse = false; return; }
    if (!in_play()) return;
    full = true;
    fitted = false;
    wheel_acc = 0;
    right_click = false;
}

bool full_open() { return full && in_play(); }

// The minimap's zoom (the keys, the wheel over it): the reach of the kind of area shown (indoors or outdoors), from
// what is shown now (a fitted room's too), x1.25 a step
void minimap_zoom(int steps) {
    if (!steps || !in_play() || !settings.map.minimap) return;
    MapSettings &m = settings.map;
    const MinimapView v = minimap_view();
    int &reach = v.indoor ? m.reach_in : m.reach;
    const int lo = v.indoor ? 20 : 30, hi = v.indoor ? 1500 : 3000;
    const float k = std::pow(1.25f, float(-std::clamp(steps, -4, 4)));
    const float from = steps > 0 ? std::min(float(reach), v.reach) : std::max(float(reach), v.reach);
    reach = std::clamp(int(std::lround(from * k)), lo, hi);
    zoom_note_until = ImGui::GetTime() + 1.5;
    save_settings();
    if (dump_test) {
        std::printf("P1_AUTOMAP_ZOOM steps=%d %s reach=%d\n", steps, v.indoor ? "indoor" : "outdoor", reach);
        std::fflush(stdout);
    }
}
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
        if (zones_mouse) {
            zones_mouse = false;
        }
    }
    // the wheel over the minimap zooms it (not while the mouse turns the camera: the cursor is not shown then)
    if (!full_open() && e.type == SDL_EVENT_MOUSE_WHEEL && mm_shown && settings.map.minimap && in_play() &&
        !(window() && SDL_GetWindowRelativeMouseMode(window())) && e.wheel.mouse_x >= mm_rect[0] && e.wheel.mouse_x < mm_rect[2] &&
        e.wheel.mouse_y >= mm_rect[1] && e.wheel.mouse_y < mm_rect[3]) {
        minimap_zoom(e.wheel.integer_y * (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1));
        return true;
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
        if (e.button.button == SDL_BUTTON_LEFT) {
            // the battle zones' button: held, not a drag
            if (zb_shown && e.button.x >= zb_rect[0] && e.button.x < zb_rect[2] && e.button.y >= zb_rect[1] && e.button.y < zb_rect[3]) {
                zones_mouse = true;
                return true;
            }
            dragging = true;
        }
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
    mm_shown = false; // (until draw() draws it again)
    if (shots && (shot_kind || shot_latched))
        return true;
    if (!in_play()) return false;
    if (shots && (newest.flags & EL_AM_PLAYER_TRUTH)) return false;
    return full || settings.map.minimap || settings.map.cheat_danger;
}

void draw() {
    float g[4];
    if (!game_rect(g)) return;
    ensure_reach();
    if (fog_dirty && (fog_tri.size() != plan.tris.size() || dump_test || ImGui::GetTime() - fog_built_at > 0.25)) build_fog();
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
    if (!full && settings.map.cheat_danger) draw_danger(dl, g);
    // (a test's pictures: the minimap as the player sees it, 84 ticks into the area: am_MM_SS_mini.png, and where it
    // is in the picture, am_MM_SS_mini.json, for tools/automap_sheet.py)
    static uint32_t mini_done;
    if (shots && !full && settings.map.minimap && mm_shown && samples_on_plan >= 84 && mini_done != plan.serial &&
        !renderer()->dump_pending()) {
        mini_done = plan.serial;
        char name[64];
        std::snprintf(name, sizeof name, "am_%02d_%02d_mini", plan.map, plan.sub);
        renderer()->request_dump(name);
        const char *dir = std::getenv("ELTALE_PRESENT_DUMP");
        char path[1024];
        std::snprintf(path, sizeof path, "%s/%s.json", dir ? dir : ".", name);
        if (FILE *f = std::fopen(path, "w")) {
            const MinimapView v = minimap_view();
            std::fprintf(f, "{\"rect\": [%.1f, %.1f, %.1f, %.1f], \"indoor\": %d, \"fitted\": %d, \"reach\": %.1f}\n", mm_rect[0],
                         mm_rect[1], mm_rect[2], mm_rect[3], v.indoor, v.fitted, v.reach);
            std::fclose(f);
        }
    }
}

void settings_page() {
    MapSettings &m = settings.map;
    bool changed = false;
    auto key_of = [](int action, const char *none) {
        for (int s = 0; s < BIND_SLOTS - 1; s++)
            if (settings.controls.bind[action][s]) return binding_label(settings.controls.bind[action][s]);
        return std::string(none);
    };
    ImGui::TextWrapped("The area map: the floor plan of the place you are in. Open it with the Map key (%s; Controls page). "
                       "Wheel: zoom, drag: move, right click: fit, point at a mark to name it, the key or Esc: close.",
                       key_of(ACT_MAP, "none").c_str());
    ImGui::Spacing();
    changed |= ImGui::Checkbox("Pause the game while the map is open", &m.pause);
    ImGui::TextDisabled("(the game makes its own music: paused, it is silent)");
    changed |= ImGui::SliderInt("Map background", &m.background, 40, 100, "%d%%");
    changed |= ImGui::Checkbox("Doors and ways out", &m.doors);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Doors you can open (orange; red: locked, it names the item it needs), and the ways out with no door\n"
                          "(a white box: stairs, a passage, a road). A white arrow: it leads to another area (a field, a dungeon, another town).");
    changed |= ImGui::Checkbox("People", &m.people);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Everyone you can talk to, where they are now (pink).");
    changed |= ImGui::Checkbox("Inns, Wings, food and MP remedies", &m.services);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The innkeepers (rest and save), the people who give Traveler's Wings, bread and MP remedies (Mint Leaves,\n"
                          "Morning Dew, Smelling Salts) when you have none, and the doors to their houses: a coloured halo and the\n"
                          "icon (moon: inn, wings, loaf: food, drop: MP remedy).");
    changed |= ImGui::Checkbox("Story people", &m.story);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A light blue ring round people with something to say about the story.");
    changed |= ImGui::Checkbox("Fog of war", &m.fog);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Only what you have seen is drawn, and the marks there; kept with each save (quick saves too).");
    changed |= ImGui::Checkbox("Signs", &m.signs);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Signs and other things to read; point at one for its first line.");
    ImGui::Separator();
    changed |= ImGui::Checkbox("Minimap", &m.minimap);
    const char *corners[] = { "Top right", "Bottom right", "Top left", "Bottom left" };
    changed |= ImGui::Combo("Corner", &m.corner, corners, 4);
    changed |= ImGui::SliderInt("Size", &m.size, 10, 45, "%d%% of the height");
    changed |= ImGui::SliderInt("Reach outdoors", &m.reach, 30, 3000, "%d units", ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::SliderInt("Reach indoors", &m.reach_in, 20, 1500, "%d units", ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::Checkbox("Fit small rooms", &m.fit_rooms);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A room smaller than the reach fills the minimap and stays put while you walk in it.");
    ImGui::TextDisabled("(while you play: %s / %s, or the wheel over the minimap, zoom the reach of the place you are in)",
                        key_of(ACT_MAP_ZOOM_IN, "-").c_str(), key_of(ACT_MAP_ZOOM_OUT, "-").c_str());
    changed |= ImGui::SliderInt("Opacity", &m.opacity, 20, 100, "%d%%");
    ImGui::Separator();
    ImGui::TextDisabled("Cheats (they stand in for a guide):");
    changed |= ImGui::Checkbox("Show the whole map", &m.cheat_reveal);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("No fog of war: every area drawn whole, with its marks.");
    changed |= ImGui::Checkbox("Show chests", &m.cheat_chests);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Every chest, and what it holds (gold; opened: an outline).");
    changed |= ImGui::Checkbox("Show spirits", &m.cheat_spirits);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The hidden spirits that raise an element of your choice (violet sparkle; taken: an outline).");
    changed |= ImGui::Checkbox("Show bosses", &m.cheat_bosses);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A boss waiting in the area, and how near you can go before it fights (red).");
    changed |= ImGui::Checkbox("Show story item givers", &m.cheat_givers);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("People who give you a story item, and what (a gold ring and key).");
    changed |= ImGui::Checkbox("Battle zones button", &m.cheat_zones);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A \"Battle zones\" button on the full map: hold it (or the key, %s) to shade where random battles\n"
                          "can start, and point at a zone for its monsters.", key_of(ACT_MAP_ZONES, "none").c_str());
    changed |= ImGui::Checkbox("Danger meter", &m.cheat_danger);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("By the minimap: the chance the next 50 units you walk start a battle, and how far you are toward\n"
                          "them (the thin strip). \"Safe here\" where no battle can start.");
    if (changed) save_settings();
}

} // namespace automap
} // namespace host

extern "C" ELHOST_API void ElHost_AutomapArea(const ElAmArea *area) { host::automap::receive_area(area); }
extern "C" ELHOST_API void ElHost_AutomapPlayer(const ElAmPlayer *player) { host::automap::receive_player(player); }
extern "C" ELHOST_API void ElHost_AutomapSave(int kind, uint64_t key) { host::automap::receive_save(kind, key); }
