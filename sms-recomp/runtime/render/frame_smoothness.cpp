// frame_smoothness — see the header for what this measures and, more importantly, what it cannot.

#include "frame_smoothness.h"

#include <lucent/log.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// The screen is judged in cells, not as a whole. A full-screen effect that ignores the
// interpolation moves a region of the frame on tick boundaries only; averaged over the whole image
// that is a few percent of the motion energy and reads as noise, so the whole-frame number cannot
// see it. 16x12 keeps a cell (40x37 pixels at 640x448) small enough that the HUD, a fade or a
// copy-sampling quad occupies several cells of its own.
constexpr int kGW = 16, kGH = 12;
constexpr int kCells = kGW * kGH;

// Luma is reduced to this before differencing: enough detail that a few pixels of object motion
// register, cheap enough to run on every present.
constexpr int kLW = kGW * 8, kLH = kGH * 8;   // 128 x 96

// A cell whose median motion energy is below this (in luma levels, 0..255) is STATIC: nothing moved
// there in the source either, so it carries no opinion about interpolation. Reported separately
// rather than folded into a pass count.
constexpr float kStaticFloor = 0.5f;

// A frame (or cell) is a DUPLICATE when its motion energy is below this share of the median. Not
// zero: the swapchain readback carries a little dither/rounding, and a genuine duplicate still
// lands far below any real motion.
constexpr float kDupShare = 0.05f;

// Samples kept for the classification. Capped so a long run cannot grow without bound; the cap is
// reported, because silently keeping "the first N" while claiming to describe the run is the
// truncation trap.
constexpr size_t kMaxSamples = 4096;

bool  g_on = false;
bool  g_selftest = false;
int   g_reportEvery = 240;

std::vector<float> g_prev;                 // previous frame's reduced luma, kLW*kLH
std::vector<float> g_globalEnergy;         // per consecutive-present pair
std::vector<std::vector<float>> g_cellEnergy;  // [cell][pair]
long g_presents = 0;
long g_dropped = 0;                        // samples past the cap

// The "is anything moving here" statistic is a HIGH PERCENTILE, not the median, and that is not a
// detail. The median is destroyed by the very defect being detected: when every other present is a
// duplicate, half the samples are exactly zero and the median sits at zero, so the static-scene
// guard fires and the instrument reports NO OPINION precisely on the case it exists to catch. The
// self-test caught this on its first run — which is the whole reason for having one.
// p90 still reads the motion level with up to ~85% duplicates.
float percentile_of(std::vector<float> v, float p) {
    if (v.empty()) return 0.0f;
    size_t k = (size_t)(p * (float)(v.size() - 1));
    if (k >= v.size()) k = v.size() - 1;
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

// Box-average the frame down to kLW x kLH luma. A box average rather than point sampling because
// point sampling aliases: a one-pixel-per-frame pan can miss every sample point and read as a
// duplicate, which is the exact false positive this instrument must not produce.
void reduce_luma(const uint8_t* rgba, int w, int h, std::vector<float>& out) {
    out.assign(kLW * kLH, 0.0f);
    if (w <= 0 || h <= 0) return;
    std::vector<uint32_t> count(kLW * kLH, 0);
    for (int y = 0; y < h; ++y) {
        const int cy = std::min(kLH - 1, y * kLH / h);
        const uint8_t* row = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            const int cx = std::min(kLW - 1, x * kLW / w);
            const uint8_t* p = row + (size_t)x * 4;
            const float luma = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
            out[cy * kLW + cx] += luma;
            ++count[cy * kLW + cx];
        }
    }
    for (size_t i = 0; i < out.size(); ++i)
        if (count[i] != 0) out[i] /= (float)count[i];
}

void feed_one(const uint8_t* rgba, int w, int h) {
    static std::vector<float> cur;
    reduce_luma(rgba, w, h, cur);
    ++g_presents;
    if (g_prev.size() != cur.size()) { g_prev = cur; return; }

    // Motion energy per cell: mean absolute luma change over the cell's 8x8 reduced pixels.
    float cell[kCells] = {};
    for (int ly = 0; ly < kLH; ++ly) {
        const int cy = ly / 8;
        for (int lx = 0; lx < kLW; ++lx) {
            const size_t i = (size_t)ly * kLW + lx;
            cell[cy * kGW + (lx / 8)] += std::fabs(cur[i] - g_prev[i]);
        }
    }
    float global = 0.0f;
    for (int c = 0; c < kCells; ++c) { cell[c] /= 64.0f; global += cell[c]; }
    global /= (float)kCells;

    if (g_globalEnergy.size() < kMaxSamples) {
        g_globalEnergy.push_back(global);
        if (g_cellEnergy.empty()) g_cellEnergy.resize(kCells);
        for (int c = 0; c < kCells; ++c) g_cellEnergy[c].push_back(cell[c]);
    } else {
        ++g_dropped;
    }
    g_prev = cur;
}

} // namespace

bool sbr_smooth_enabled() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SBR_SMOOTH");
        v = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
        g_on = v == 1;
        const char* s = std::getenv("SBR_SMOOTH_SELFTEST");
        g_selftest = (s != nullptr && s[0] != '\0' && s[0] != '0');
        if (const char* r = std::getenv("SBR_SMOOTH_EVERY")) g_reportEvery = std::max(1, std::atoi(r));
        if (g_on)
            lucent::info("smooth", "present-smoothness armed{}: every present is differenced "
                                   "against the previous one, per cell ({}x{})",
                         g_selftest ? " in SELF-TEST mode (each frame fed TWICE — this MUST report "
                                      "dupFrac ~0.5 and stair-step every moving cell)" : "",
                         kGW, kGH);
    }
    return g_on;
}

void sbr_smooth_feed(const uint8_t* rgba, int w, int h) {
    if (!sbr_smooth_enabled() || rgba == nullptr) return;
    feed_one(rgba, w, h);
    // SELF-TEST: feeding the same image again is precisely the defect being hunted — a present that
    // shows the previous picture. If the analyser does not light up here it cannot light up on the
    // real thing either.
    if (g_selftest) feed_one(rgba, w, h);
    if (g_reportEvery > 0 && g_presents % g_reportEvery == 0) sbr_smooth_report();
}

void sbr_smooth_report() {
    if (!sbr_smooth_enabled()) return;
    const size_t n = g_globalEnergy.size();
    if (n < 16) {
        lucent::info("smooth", "only {} present-pairs sampled — too few to classify, NOT a pass",
                     n);
        return;
    }
    const float gHi  = percentile_of(g_globalEnergy, 0.90f);
    const float gMed = percentile_of(g_globalEnergy, 0.50f);
    const float gMax = percentile_of(g_globalEnergy, 1.00f);
    // REFUSE rather than report a number. With a still scene every present is legitimately a
    // duplicate and dupFrac would read 0.5-1.0 with nothing wrong; returning that as a finding is
    // the instrument lying. Say the corpus is unusable instead — and carry the denominator, so a
    // refusal can be told apart from never having looked.
    if (gHi < kStaticFloor) {
        lucent::warn("smooth", "SCENE IS NOT MOVING — NO OPINION on interpolation. Over {} "
                               "present-pairs the frame energy was p50 {:.3f} p90 {:.3f} max "
                               "{:.3f}; p90 is below the {:.3f} luma-level floor, so at least 90% "
                               "of presents are legitimately identical and a dupFrac from this "
                               "sample would measure the scene, not the renderer. Measure during "
                               "motion (SBR_PAD_SCRIPT=\"120:STICK=0/-90\" walks the camera).",
                     n, gMed, gHi, gMax, kStaticFloor);
        return;
    }

    const float dupThresh = kDupShare * gHi;
    size_t dup = 0;
    std::vector<size_t> moving;
    for (size_t i = 0; i < n; ++i) {
        if (g_globalEnergy[i] < dupThresh) ++dup;
        else moving.push_back(i);
    }
    const double dupFrac = (double)dup / (double)n;

    lucent::info("smooth", "presents={} pairs={} | frame dupFrac {:.3f} "
                           "(0.00 = every present a new image; 0.50 = every other present is a "
                           "DUPLICATE, i.e. 30fps shown twice) | frame energy p50 {:.2f} p90 {:.2f}",
                 g_presents, n, dupFrac, gMed, gHi);

    // ---- PER CELL: the ALTERNATION SCORE ---------------------------------------------------
    // The first version of this counted, per cell, the share of frames on which the cell stood
    // still while the frame moved. The self-test destroyed it: in a mostly-static plaza 85 of 131
    // moving cells scored 89%, because a cell that is genuinely still except for occasional bursts
    // is indistinguishable, by that measure, from a cell that is duplicated every other present.
    // Burstiness of the CONTENT read as a defect of the RENDERER.
    //
    // What actually separates them is the SHAPE of the series, not how much of it is near zero: a
    // duplicated cell alternates big/small on ADJACENT pairs. So score each adjacent pair of diffs
    // on its own — |d0-d1| / (d0+d1) — and average. 0.0 = consecutive presents move the cell by the
    // same amount (a genuine interpolation); 1.0 = every other present does not move it at all.
    // Phase-free by construction: it never assumes which index is the duplicate, so a hitch that
    // breaks even/odd parity cannot make a duplicated stream read as smooth.
    //
    // This score is CONTENT-DEPENDENT and is not a pass/fail on its own — a bursty cell scores high
    // with a perfect renderer. It is meant to be DIFFED between a lerp-off and a lerp-on run of the
    // same scene: a cell whose score rises only when interpolation is on is the region ignoring it.
    int staticCells = 0, movingCells = 0;
    std::vector<float> altScore((size_t)kCells, -1.0f);
    for (int c = 0; c < kCells; ++c) {
        const std::vector<float>& e = g_cellEnergy[c];
        if (percentile_of(e, 0.90f) < kStaticFloor) { ++staticCells; continue; }
        ++movingCells;
        double acc = 0.0; long m = 0;
        for (size_t i = 0; i + 1 < e.size(); ++i) {
            const float s = e[i] + e[i + 1];
            if (s < 2.0f * kStaticFloor) continue;   // both near zero: no opinion from this pair
            acc += std::fabs(e[i] - e[i + 1]) / s;
            ++m;
        }
        altScore[c] = m != 0 ? (float)(acc / (double)m) : -1.0f;
        if (m == 0) { --movingCells; ++staticCells; }
    }
    double altSum = 0.0; int altN = 0;
    for (float v : altScore) if (v >= 0.0f) { altSum += v; ++altN; }
    lucent::info("smooth", "cells {} = {} judged, {} static/no-opinion | mean alternation {:.3f} "
                           "(0.0 = consecutive presents move a cell equally; 1.0 = every other "
                           "present does not move it at all)",
                 kCells, movingCells, staticCells, altN ? altSum / altN : 0.0);

    // The whole grid, as a map, so two runs can be compared directly instead of through a summary
    // that has already thrown away where the problem is. Digit = alternation score * 10; '.' = a
    // cell with no opinion.
    for (int cy = 0; cy < kGH; ++cy) {
        std::string row;
        for (int cx = 0; cx < kGW; ++cx) {
            const float v = altScore[cy * kGW + cx];
            row += v < 0.0f ? '.' : (char)('0' + std::min(9, (int)(v * 10.0f)));
        }
        lucent::info("smooth", "  alt |{}|", row);
    }

    std::string dropped;
    if (g_dropped != 0)
        dropped = "; " + std::to_string(g_dropped) + " samples past the " +
                  std::to_string(kMaxSamples) + "-pair cap were DROPPED";
    lucent::info("smooth", "BLIND SPOTS: measures EVENNESS, not correctness — a wrong-but-even "
                           "interpolation (bad alpha, mis-paired object) scores perfectly here; {} "
                           "of {} cells were static and carry NO opinion; a cell reads moving even "
                           "when only one object's edge crosses it{}",
                 staticCells, kCells, dropped);
}
