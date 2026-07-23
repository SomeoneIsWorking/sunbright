// scene.cpp — the interpolated scene (see scene.h for why this replaced the tick-driven approach).

#include "scene.h"

#include <lucent/log.h>

#include <algorithm>
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

void lerp_mtx(float out[12], const float a[12], const float b[12], float t) {
    // Component-wise on a model x view matrix. Correct for translation and near-correct for the
    // rotation of a small per-tick delta; a proper decompose/slerp is the upgrade if fast spins
    // ever show shear, and is deliberately deferred rather than guessed at now.
    for (int i = 0; i < 12; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
}

} // namespace

void sbr_scene_begin_tick() {
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

    for (const auto& d : g_cur.items) {
        if (d.geom == 0) continue;   // geometry not decoded yet

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
        // TODO(geometry): submit d.geom transformed by m and proj. The decode lands next; the
        // interpolation above is complete and independent of it.
        (void)proj;
    }
    return alpha;
}

int sbr_scene_last_count() { return g_lastCount; }
int sbr_scene_matched_count() { return g_matched; }
