// motion_truth.cpp — HOW FAR DOES A REAL OBJECT MOVE IN ONE TICK? The control for the pairing
// audit's central claim.
//
// The interpolation report prints a distribution of paired-draw object motion and says of it:
// "anything from [10,100) up is a pose no object reaches in 1/30 s, so those counts are the
// mispairings". On a plaza run that sentence condemns 21,866 draws — 6.8% of everything that
// paired — and the whole judgement rests on a threshold nobody measured. It is a CLAIM about the
// game's own units, made by a comparison instrument, about itself.
//
// So measure it against something whose motion is not in question. gpMario's world position is read
// straight out of guest memory once per simulation tick, and its per-tick displacement is bucketed
// with the SAME edges as the paired-draw histogram. Mario is the fastest thing the player controls;
// if his ordinary running lands in [10,100), then that bucket is what motion looks like and the
// audit has been calling normal gameplay a defect. If he never leaves [0,10), the threshold is
// vindicated and the tail is real mispairing.
//
// The two instruments measure the same quantity — world units between consecutive simulation ticks —
// which is the thing this project keeps getting wrong when it compares two numbers. One reads a
// guest global, the other reads matrices aurora recorded; both are per tick, both are world space,
// and neither is the other's denominator.
//
// It cannot answer everything, and the negative is stated here rather than left implied: this is
// ONE object. It bounds what a player-controlled character does, and says nothing about a warping
// camera-target, a scripted teleport, or an effect that respawns at a new place — which is exactly
// why the answer it gives is a floor for "possible", not a ceiling.

#include "motion_truth.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cmath>

namespace {

// gpMario, the same pointer native_frame's position report reads. The position is the first member.
constexpr u32 GPMARIO_PTR = 0x8040E10C;

constexpr int kBuckets = 7;   // [0,0.1) [0.1,1) [1,10) [10,100) [100,1k) [1k,10k) [10k,inf)
long g_hist[kBuckets] = {};
long g_samples = 0;
double g_max = 0.0;
double g_sum = 0.0;
bool g_have = false;
float g_prev[3] = {};
long g_unreadable = 0;

float guest_f32(u32 addr) {
    const u32 bits = sb_r32(addr);
    float f;
    __builtin_memcpy(&f, &bits, sizeof f);
    return f;
}

} // namespace

void sbr_motion_truth_tick() {
    if (!sb_ram_fast(GPMARIO_PTR)) { ++g_unreadable; return; }
    const u32 mario = sb_r32(GPMARIO_PTR);
    if (mario == 0 || !sb_ram_fast(mario + 8)) { ++g_unreadable; return; }
    const float x = guest_f32(mario), y = guest_f32(mario + 4), z = guest_f32(mario + 8);
    if (g_have) {
        const double dx = x - g_prev[0], dy = y - g_prev[1], dz = z - g_prev[2];
        const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
        int bucket = 0;
        for (double edge = 0.1; bucket < kBuckets - 1 && d >= edge; edge *= 10.0) {
            ++bucket;
        }
        ++g_hist[bucket];
        ++g_samples;
        g_sum += d;
        if (d > g_max) { g_max = d; }
    }
    g_prev[0] = x; g_prev[1] = y; g_prev[2] = z;
    g_have = true;
}

void sbr_motion_truth_report() {
    if (g_samples == 0) {
        // REFUSE rather than print an empty distribution: "Mario never moved" and "this never read
        // his position" are the same zeros, and only one of them is a fact about the game.
        lucent::warn("truth", "gpMario's position was never sampled ({} unreadable attempt(s)), so "
                              "there is NO ground truth for per-tick motion this run. Any claim "
                              "about which motion bucket is 'impossible' is unsupported here.",
                     g_unreadable);
        return;
    }
    lucent::info("truth",
                 "GROUND TRUTH — gpMario's own world motion per simulation tick over {} tick(s): "
                 "mean {:.3f} max {:.3f} world units. Distribution, SAME buckets as the paired-draw "
                 "histogram: [0,0.1) {} | [0.1,1) {} | [1,10) {} | [10,100) {} | [100,1k) {} | "
                 "[1k,10k) {} | [10k,inf) {}. Read the audit's tail against THIS: a bucket Mario "
                 "occupies while running is not evidence of mispairing.",
                 g_samples, g_sum / (double)g_samples, g_max, g_hist[0], g_hist[1], g_hist[2],
                 g_hist[3], g_hist[4], g_hist[5], g_hist[6]);
}
