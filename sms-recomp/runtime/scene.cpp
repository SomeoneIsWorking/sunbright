// scene.cpp — the interpolated scene (see scene.h for why this replaced the tick-driven approach).

#include "scene.h"

#include <lucent/log.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

struct Snapshot {
    std::vector<SbrDrawable> items;
    std::unordered_map<uint64_t, uint32_t> index;   // key -> position in items
    double time = 0.0;
    bool valid = false;
};

Snapshot g_cur, g_prev, g_building;
int g_lastCount = 0, g_matched = 0;

// Tick period, learned from the observed interval between snapshots rather than assumed to be
// 1/30 — a load hitch or a 60 Hz scene would otherwise make the interpolation run at the wrong
// speed. Smoothed, because one long tick should not permanently skew it.
double g_tickPeriod = 1.0 / 30.0;

// ---------------------------------------------------------------------------------------------
// Geometry cache. Handles are 1-based so 0 keeps meaning "no geometry".

struct Area {
    uint64_t key;
    float    frac;      // fraction of the viewport covered by the projected bounding box
    int      verts;
    float    nearestW;  // smallest clip w = closest approach to the camera
};
std::vector<Area> g_area;

struct Geom {
    std::vector<SbrGeomVert> verts;
    bool multislot = false;
};
std::vector<Geom> g_geom{1};                       // [0] unused: handle 0 = none
std::unordered_map<uint64_t, uint32_t> g_geomIndex;
int g_multislot = 0;

float g_proj[16];
bool  g_projValid = false;
bool  g_projThisTick = false;

// The renderer's scratch vertex buffer, kept across frames so a 60 Hz render does not allocate.
std::vector<SbrVertex> g_out;

// SBR_RENDER_DEPTHVIZ=1 — shade by depth instead of per-object colour.
const bool g_skipSkinned = [] {
    const char* e = std::getenv("SBR_RENDER_SKIP_SKINNED");
    return e != nullptr && e[0] != '\0' && e[0] != '0';
}();

const bool g_depthViz = [] {
    const char* e = std::getenv("SBR_RENDER_DEPTHVIZ");
    return e != nullptr && e[0] != '\0' && e[0] != '0';
}();

void lerp_mtx(float out[12], const float a[12], const float b[12], float t) {
    // Component-wise on a model x view matrix. Correct for translation and near-correct for the
    // rotation of a small per-tick delta; a proper decompose/slerp is the upgrade if fast spins
    // ever show shear, and is deliberately deferred rather than guessed at now.
    for (int i = 0; i < 12; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
}

} // namespace

bool sbr_scene_has_geometry(uint64_t key) { return g_geomIndex.count(key) != 0; }

uint32_t sbr_scene_intern_geometry(uint64_t key, const SbrGeomVert* verts, int count) {
    if (const auto it = g_geomIndex.find(key); it != g_geomIndex.end()) return it->second;

    const uint32_t id = (uint32_t)g_geom.size();
    g_geom.push_back(Geom{});
    Geom& g = g_geom.back();
    g.verts.assign(verts, verts + count);

    // A shape whose vertices select more than one matrix slot is SKINNED. The drawable carries one
    // matrix, so such a mesh renders with the wrong transform on every vertex outside the first
    // slot. Count it instead of hiding it: the per-vertex matrix path is the next step, and this
    // number is how big it is.
    for (int i = 1; i < count; ++i)
        if (verts[i].slot != verts[0].slot) { g.multislot = true; ++g_multislot; break; }

    g_geomIndex.emplace(key, id);
    return id;
}

void sbr_scene_set_projection(const float m[16]) {
    // FIRST perspective of the tick only. The game sets several — the main camera, then the
    // projections screen effects rebuild for their own passes — and the last one to be set is
    // whichever effect ran most recently, not the one the scene was drawn through. The main
    // camera's is established before the scene draw, so taking the first is what matches the
    // geometry being captured.
    if (g_projThisTick) return;
    std::memcpy(g_proj, m, sizeof g_proj);
    static int once = 0;
    if (once < 2) {
        ++once;
        lucent::info("nrender", "projection rows: [{:.4f} {:.4f} {:.4f} {:.4f}] "
                                "[{:.4f} {:.4f} {:.4f} {:.4f}] [{:.4f} {:.4f} {:.4f} {:.4f}] "
                                "[{:.4f} {:.4f} {:.4f} {:.4f}]",
                     m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                     m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    }
    g_projValid = true;
    g_projThisTick = true;
}
bool sbr_scene_has_projection() { return g_projValid; }
const float* sbr_scene_projection() { return g_projValid ? g_proj : nullptr; }

double sbr_scene_now() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
int  sbr_scene_multislot_count() { return g_multislot; }

void sbr_scene_report_zmodes() {
    // Which depth states the scene actually uses. If this shows ONE state, per-material depth is
    // not what is occluding the plaza and the theory is wrong — cheaper to check than to assume.
    std::unordered_map<uint32_t, int> hist;
    for (const auto& d : g_cur.items) {
        const uint32_t k = (uint32_t)d.depth.test << 16 | (uint32_t)d.depth.func << 8 | d.depth.write;
        ++hist[k];
    }
    for (const auto& [k, n] : hist)
        lucent::info("nrender", "  zmode test={} func={} write={} -> {} drawables",
                     (k >> 16) & 1, (k >> 8) & 7, k & 1, n);
}

void sbr_scene_report_largest(int n) {
    std::vector<Area> a = g_area;
    std::sort(a.begin(), a.end(), [](const Area& x, const Area& y) { return x.frac > y.frac; });
    for (int i = 0; i < n && i < (int)a.size(); ++i)
    {
        // Report the drawable's COLOUR too. The colour is a pure function of the key, so a dominant
        // colour in the output identifies its drawable exactly — turning "something covers the
        // screen" into "THIS shape covers the screen", without another round of hypotheses.
        uint64_t z = a[i].key + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        const uint32_t hh = (uint32_t)(z ^ (z >> 31));
        const int cr = (int)(255.0f * (0.35f + 0.65f * (float)((hh >> 16) & 0xFF) / 255.0f));
        const int cg = (int)(255.0f * (0.35f + 0.65f * (float)((hh >> 8) & 0xFF) / 255.0f));
        const int cb = (int)(255.0f * (0.35f + 0.65f * (float)(hh & 0xFF) / 255.0f));
        lucent::info("nrender", "  occluder #{}: shape 0x{:08x} el {} covers {:5.1f}%, {} verts, "
                                "nearest w={:.1f}, colour ({},{},{})",
                     i + 1, (uint32_t)(a[i].key >> 16), (uint32_t)(a[i].key & 0xFFFF),
                     100.0f * a[i].frac, a[i].verts, a[i].nearestW, cr, cg, cb);
    }
}

void sbr_scene_begin_tick() {
    g_projThisTick = false;
    g_building.items.clear();
    g_building.index.clear();
}

void sbr_scene_add(const SbrDrawable& d) {
    g_building.index.emplace(d.key, (uint32_t)g_building.items.size());
    g_building.items.push_back(d);
}

void sbr_scene_end_tick() {
    g_prev = std::move(g_cur);
    g_cur = std::move(g_building);
    g_building = Snapshot{};

    // Stamp with the monotonic clock the renderer also reads.
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_cur.time = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    g_cur.valid = true;

    if (g_prev.valid) {
        const double dt = g_cur.time - g_prev.time;
        // Ignore absurd intervals (a load hitch, or the first tick after a stage change) rather than
        // letting one of them poison the period the interpolation runs at.
        if (dt > 0.002 && dt < 0.2) g_tickPeriod = g_tickPeriod * 0.9 + dt * 0.1;
    }

    g_lastCount = (int)g_cur.items.size();
    g_matched = 0;
    if (g_prev.valid)
        for (const auto& d : g_cur.items)
            if (g_prev.index.count(d.key)) ++g_matched;
}

float sbr_scene_render(double now_seconds, const float proj[16]) {
    if (!g_cur.valid) return 0.0f;

    // How far between the two snapshots this display frame falls. Derived from the WALL CLOCK, so
    // the render rate is independent of the tick rate — the whole point of the rework.
    float alpha = 1.0f;
    if (g_prev.valid && g_tickPeriod > 0.0) {
        alpha = (float)((now_seconds - g_cur.time) / g_tickPeriod);
        // Render one tick BEHIND: the scene is drawn between prev and cur, so it is always
        // interpolating between two KNOWN states rather than extrapolating past cur into a state the
        // game has not simulated. Extrapolation is what makes objects overshoot and snap back.
        alpha = std::clamp(alpha, 0.0f, 1.0f);
    }

    if (proj == nullptr) return alpha;   // nothing to place geometry with

    g_area.clear();
    for (const auto& d : g_cur.items) {
        if (d.geom == 0 || d.geom >= g_geom.size()) continue;   // no geometry decoded for this one
        const Geom& g = g_geom[d.geom];
        if (g.verts.empty()) continue;
        // SBR_RENDER_SKIP_SKINNED=1 — omit shapes whose vertices select more than one matrix slot.
        // Those are the shapes whose single per-drawable matrix is KNOWN to be wrong (the slot read
        // from a multi-matrix J3DShapeMtx is not a slot at all), so this isolates how much of the
        // picture their garbage transforms are responsible for.
        if (g_skipSkinned && g.multislot) continue;

        float m[12];
        const auto it = g_prev.valid ? g_prev.index.find(d.key) : g_prev.index.end();
        if (it != g_prev.index.end()) {
            lerp_mtx(m, g_prev.items[it->second].mtx, d.mtx, alpha);
        } else {
            // New this tick: no previous state to come from. Draw it AT its current transform rather
            // than interpolating from whatever memory happened to hold — the same provenance rule
            // that the old approach needed a special case for, here a natural consequence.
            std::memcpy(m, d.mtx, sizeof m);
        }

        // Bring-up colouring: a deterministic hue per drawable so distinct objects are
        // distinguishable in the A/B against aurora. Materials and TEV are a later step; this is
        // explicitly NOT an attempt at the game's shading.
        // splitmix64 finaliser: a plain multiply truncated to 32 bits left the low bits zero for
        // every key whose element index was 0 — which is most shapes — so two of the three channels
        // were constant and the picture was unreadable.
        uint64_t z = d.key + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        const uint32_t h = (uint32_t)(z ^ (z >> 31));
        const float cr = 0.35f + 0.65f * (float)((h >> 16) & 0xFF) / 255.0f;
        const float cg = 0.35f + 0.65f * (float)((h >> 8) & 0xFF) / 255.0f;
        const float cb = 0.35f + 0.65f * (float)(h & 0xFF) / 255.0f;

        // Per-DRAWABLE buffer: depth state is a per-material property, so each drawable is
        // submitted under its own state. The backend merges consecutive runs that share a state,
        // so an unchanging scene still collapses to very few draws.
        g_out.clear();
        float nlo[2] = {1e30f, 1e30f}, nhi[2] = {-1e30f, -1e30f};
        float nearestW = 1e30f;
        for (const SbrGeomVert& v : g.verts) {
            // model -> view: the drawable's 3x4 is already model x view (J3D concats the view in
            // viewCalc), so this single multiply lands the vertex in eye space.
            const float vx = m[0] * v.x + m[1] * v.y + m[2]  * v.z + m[3];
            const float vy = m[4] * v.x + m[5] * v.y + m[6]  * v.z + m[7];
            const float vz = m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11];

            // view -> clip through the game's own projection.
            //
            // GC clip depth is NOT [0,1]. The game's own matrix (measured) has row 2 = [0, 0, 0,
            // -near], so z/w = -near/w: exactly -1 at the near plane and approaching 0 at infinity.
            // The convention is [-1, 0], near to far — an infinite-far projection.
            //
            // Vulkan (and so SDL3 GPU) wants [0, 1] and CLIPS anything with z < 0, so feeding GC's
            // z straight through discarded every surface near the camera and left only the distant
            // ones. That is what painted the sky and terrain over the whole plaza, and what made
            // the depth visualisation read 0 everywhere.
            SbrVertex o{};
            const float* P = d.proj;
            o.x = P[0]  * vx + P[1]  * vy + P[2]  * vz + P[3];
            o.y = P[4]  * vx + P[5]  * vy + P[6]  * vz + P[7];
            o.z = P[8]  * vx + P[9]  * vy + P[10] * vz + P[11];
            o.w = P[12] * vx + P[13] * vy + P[14] * vz + P[15];
            // GC clip space has +Y up; the backend's NDC has +Y down. Flipping here keeps the
            // convention change at the ONE place the two spaces meet.
            o.y = -o.y;
            // [-1,0] -> [0,1]: adding w maps near (-w) to 0 and far (0) to w. Same shape as the
            // standard GL->Vulkan depth conversion, and derived from the matrix above rather than
            // fitted to make the picture look right. GC's ORTHO shares the convention (C_MTXOrtho
            // maps near to -1 and far to 0), so the same correction applies to both.
            o.z += o.w;
            if (g_depthViz) {
                // Grayscale by normalised depth: near = black, far = white. If a surface that
                // should be distant paints itself dark (or vice versa) the transform for that mesh
                // is wrong, which per-object colours cannot show.
                const float dz = (o.w > 1e-4f) ? std::clamp(o.z / o.w, 0.0f, 1.0f) : 1.0f;
                o.r = o.g = o.b = dz;
            } else {
                o.r = cr; o.g = cg; o.b = cb;
            }
            o.a = 1.0f;
            g_out.push_back(o);

            if (o.w > 1e-4f) {
                const float sx = o.x / o.w, sy = o.y / o.w;
                nlo[0] = std::min(nlo[0], sx); nhi[0] = std::max(nhi[0], sx);
                nlo[1] = std::min(nlo[1], sy); nhi[1] = std::max(nhi[1], sy);
                nearestW = std::min(nearestW, o.w);
            }
        }
        if (!g_out.empty()) sbr_render_tris(g_out.data(), (int)(g_out.size() / 3) * 3, d.depth);

        if (nhi[0] > nlo[0]) {
            // Clamp to the viewport before measuring: an off-screen quad's raw NDC extent is huge
            // but it covers nothing, and ranking by that would blame the wrong drawable.
            const float w0 = std::min(nhi[0], 1.0f) - std::max(nlo[0], -1.0f);
            const float h0 = std::min(nhi[1], 1.0f) - std::max(nlo[1], -1.0f);
            if (w0 > 0.0f && h0 > 0.0f)
                g_area.push_back(Area{d.key, w0 * h0 * 0.25f, (int)g.verts.size(), nearestW});
        }
    }

    return alpha;
}

void sbr_scene_translation_bounds(float lo[3], float hi[3], float* medianDist) {
    for (int i = 0; i < 3; ++i) { lo[i] = 1e30f; hi[i] = -1e30f; }
    std::vector<float> d;
    d.reserve(g_cur.items.size());
    for (const auto& it : g_cur.items) {
        const float t[3] = {it.mtx[3], it.mtx[7], it.mtx[11]};
        for (int i = 0; i < 3; ++i) { lo[i] = std::min(lo[i], t[i]); hi[i] = std::max(hi[i], t[i]); }
        d.push_back(std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]));
    }
    if (d.empty()) { *medianDist = 0.0f; return; }
    std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
    *medianDist = d[d.size() / 2];
}

int sbr_scene_last_count() { return g_lastCount; }
int sbr_scene_matched_count() { return g_matched; }
