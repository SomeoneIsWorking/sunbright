// render_compare.cpp — in-process A/B of the native render against the aurora oracle.
// See render_compare.h for why this replaced the file-based harness.

#include "render_compare.h"

#include "frame_smoothness.h"

#include <aurora/aurora.h>
#include <lucent/log.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

// Both images are resampled onto this grid before anything is measured. Small enough that the
// per-frame cost is irrelevant, large enough that building silhouettes survive.
constexpr int kGW = 320, kGH = 224;

// Fraction of pixels treated as "edge" in each image. Taken as a PERCENTILE of each image's own
// gradient magnitude rather than a fixed threshold, so neither exposure nor the native path's flat
// shading biases the comparison.
constexpr float kEdgeFraction = 0.15f;

bool g_on = false;
bool g_registered = false;
int  g_every = 60;

struct Frame {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    bool valid = false;
};
Frame g_native;
uint8_t g_clear[3] = {0, 0, 0};
long g_captures = 0;

// Every scored frame that had geometry, so the report can be a distribution rather than a sample.
struct Score { double iou, corr; };
std::vector<Score> g_scored;

// Point-sample onto the common grid. Nearest-neighbour is deliberate: averaging would blur exactly
// the thin structures (railings, poles, window frames) the edge metric is there to compare.
void to_grid_luma(const uint8_t* src, int sw, int sh, std::vector<float>& out) {
    out.resize(kGW * kGH);
    for (int y = 0; y < kGH; ++y) {
        const int sy = (int)((int64_t)y * sh / kGH);
        for (int x = 0; x < kGW; ++x) {
            const int sx = (int)((int64_t)x * sw / kGW);
            const uint8_t* p = src + ((size_t)sy * sw + sx) * 4;
            out[y * kGW + x] = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
        }
    }
}

// Sobel magnitude, then keep the strongest kEdgeFraction as the edge mask.
void edge_mask(const std::vector<float>& lum, std::vector<uint8_t>& mask) {
    std::vector<float> mag((size_t)kGW * kGH, 0.0f);
    for (int y = 1; y < kGH - 1; ++y) {
        for (int x = 1; x < kGW - 1; ++x) {
            const auto L = [&](int dx, int dy) { return lum[(y + dy) * kGW + (x + dx)]; };
            const float gx = -L(-1, -1) - 2 * L(-1, 0) - L(-1, 1) + L(1, -1) + 2 * L(1, 0) + L(1, 1);
            const float gy = -L(-1, -1) - 2 * L(0, -1) - L(1, -1) + L(-1, 1) + 2 * L(0, 1) + L(1, 1);
            mag[y * kGW + x] = std::sqrt(gx * gx + gy * gy);
        }
    }
    std::vector<float> sorted = mag;
    const size_t k = (size_t)((1.0f - kEdgeFraction) * (float)sorted.size());
    std::nth_element(sorted.begin(), sorted.begin() + k, sorted.end());
    const float thr = std::max(sorted[k], 1.0f);   // floor: a flat image has no edges, not all edges
    mask.assign((size_t)kGW * kGH, 0);
    for (size_t i = 0; i < mag.size(); ++i) mask[i] = mag[i] >= thr ? 1 : 0;
}

float pearson(const std::vector<float>& a, const std::vector<float>& b) {
    double ma = 0, mb = 0;
    for (size_t i = 0; i < a.size(); ++i) { ma += a[i]; mb += b[i]; }
    ma /= (double)a.size();
    mb /= (double)b.size();
    double num = 0, da = 0, db = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double x = a[i] - ma, y = b[i] - mb;
        num += x * y; da += x * x; db += y * y;
    }
    if (da <= 0.0 || db <= 0.0) return 0.0f;
    return (float)(num / std::sqrt(da * db));
}

// SBR_AB_SELFTEST=1 scores aurora against ITSELF. A metric that cannot report a perfect match on
// identical input is not measuring what it claims, and a bad score from it would be indistinguishable
// from a bad render — so the instrument is validated against a known-positive before its verdicts on
// the native path are believed.
bool selftest() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SBR_AB_SELFTEST");
        v = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    return v == 1;
}

// ---- Operation attribution ----
// Variants submitted for the CURRENT frame, scored against the same aurora frame as the baseline.
// `seq` is the baseline this variant belongs to. Aurora's sink fires from its own end-of-frame,
// which can land BETWEEN the baseline submit and the sweep — so position in the queue does not
// establish which frame a variant came from, and scoring on position compared variants against
// the NEXT aurora frame. That is what made the control:no-op ablation score -11.8 instead of 0.
struct Variant { int id = 0; uint64_t seq = 0; std::string name; std::vector<uint8_t> rgba;
                 int w = 0, h = 0; };
std::vector<Variant> g_variants;
struct VarAcc { std::string name; double iou = 0, corr = 0; long n = 0; };
std::map<int, VarAcc> g_varAcc;
uint64_t g_nativeSeq = 0;      // bumped per baseline submit; variants carry the value they saw
long g_variantDropped = 0;     // variant sets that never met their baseline — reported, not hidden

bool ablate() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SBR_ABLATE");
        v = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    return v == 1;
}

// Score one native buffer against an aurora frame with the SAME metric as the baseline.
void score_against(const uint8_t* nat, int nw, int nh, const uint8_t* aur, int aw, int ah,
                   double& iou, double& corr) {
    std::vector<float> ln, la;
    to_grid_luma(nat, nw, nh, ln);
    to_grid_luma(aur, aw, ah, la);
    std::vector<uint8_t> en, ea;
    edge_mask(ln, en);
    edge_mask(la, ea);
    long inter = 0, uni = 0;
    for (size_t i = 0; i < en.size(); ++i) {
        if (en[i] || ea[i]) ++uni;
        if (en[i] && ea[i]) ++inter;
    }
    iou  = uni ? 100.0 * (double)inter / (double)uni : 0.0;
    corr = pearson(ln, la);
}

void on_aurora_frame(const uint8_t* rgba, uint32_t w, uint32_t h, void*) {
    // Smoothness is a property of CONSECUTIVE presents, so it must see every one. When it is armed
    // the sink cadence is forced to 1 and the A/B below is gated by its own counter instead —
    // scoring every frame would otherwise make the two instruments fight over the sink.
    sbr_smooth_feed(rgba, (int)w, (int)h);
    if (sbr_smooth_enabled() && g_every > 1) {
        static int tick = 0;
        if (++tick % g_every != 0) return;
    }
    if (!sbr_compare_enabled()) return;
    if (selftest()) {
        g_native.rgba.assign(rgba, rgba + (size_t)w * h * 4);
        g_native.w = (int)w;
        g_native.h = (int)h;
        g_native.valid = true;
        g_clear[0] = g_clear[1] = g_clear[2] = 0xFF;   // unused by the structural metrics
    }
    if (!g_native.valid) return;

    // Native coverage against the EXACT clear colour — no inference, so this number means what it
    // says even when one object covers most of the frame.
    long lit = 0;
    const size_t npx = (size_t)g_native.w * g_native.h;
    for (size_t i = 0; i < npx; ++i) {
        const uint8_t* p = &g_native.rgba[i * 4];
        if (p[0] != g_clear[0] || p[1] != g_clear[1] || p[2] != g_clear[2]) ++lit;
    }

    std::vector<float> ln, la;
    to_grid_luma(g_native.rgba.data(), g_native.w, g_native.h, ln);
    to_grid_luma(rgba, (int)w, (int)h, la);

    std::vector<uint8_t> en, ea;
    edge_mask(ln, en);
    edge_mask(la, ea);
    long inter = 0, uni = 0;
    for (size_t i = 0; i < en.size(); ++i) {
        if (en[i] || ea[i]) ++uni;
        if (en[i] && ea[i]) ++inter;
    }

    ++g_captures;
    const double iou  = uni ? 100.0 * (double)inter / (double)uni : 0.0;
    const double corr = pearson(ln, la);
    lucent::info("ab", "#{} native {}x{} vs aurora {}x{} | geom {:.1f}% | edgeIoU {:.1f}% | "
                       "lumaCorr {:+.3f}",
                 g_captures, g_native.w, g_native.h, w, h, 100.0 * (double)lit / (double)npx,
                 iou, corr);

    // RUNNING SUMMARY. A single frame's score is not comparable between runs: consecutive frames
    // differ in animation phase, and the spread between them turned out to be as large as the
    // changes being measured — so reading one number as progress is measuring noise. Accumulate
    // from the first frame that has geometry (earlier ones are the loading screen and would drag
    // every mean toward zero) and report the mean, the best, and the sample count.
    if (lit > 0) {
        g_scored.push_back({iou, corr});
        double si = 0.0, sc = 0.0, bi = 0.0;
        for (const auto& s : g_scored) { si += s.iou; sc += s.corr; bi = std::max(bi, s.iou); }
        const double n = (double)g_scored.size();
        lucent::info("ab", "    mean over {} scored frames: edgeIoU {:.1f}% (best {:.1f}%), "
                           "lumaCorr {:+.3f}", g_scored.size(), si / n, bi, sc / n);
        // A FIXED-N comparison point, emitted exactly once. The running mean drifts several points
        // with the frame COUNT alone, so two runs of different length are not comparable — reading
        // them as a before/after produced a wrong conclusion in this project once already. Compare
        // runs on THIS line and nothing else; SBR_AB_AT moves the point.
        static const size_t kAt = [] {
            const char* e = std::getenv("SBR_AB_AT");
            return (size_t)(e != nullptr ? std::strtoul(e, nullptr, 10) : 59);
        }();
        if (g_scored.size() == kAt)
            lucent::info("ab", "=== COMPARABLE @ N={}: edgeIoU {:.2f}% lumaCorr {:+.4f} === "
                               "(compare runs on THIS line; means at different N are not "
                               "comparable)", kAt, si / n, sc / n);
    }
    // Every variant of THIS frame against THIS aurora frame — the whole point of doing the sweep
    // in-process. A variant that scores higher than the baseline names an operation this port is
    // getting wrong, because replacing it with a neutral reference moved the frame TOWARD aurora.
    if (lit > 0)
        for (const Variant& v : g_variants) {
            if (v.seq != g_nativeSeq) { ++g_variantDropped; continue; }
            double vi = 0, vc = 0;
            score_against(v.rgba.data(), v.w, v.h, rgba, (int)w, (int)h, vi, vc);
            VarAcc& a = g_varAcc[v.id];
            a.name = v.name;
            a.iou += vi; a.corr += vc; ++a.n;
        }
    g_variants.clear();
    g_native.valid = false;   // consume: never score the same native frame against two oracles
}

} // namespace

bool sbr_compare_enabled() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SBR_AB");
        v = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
        if (const char* n = std::getenv("SBR_AB_EVERY")) g_every = std::max(1, std::atoi(n));
        g_on = v == 1;
    }
    return g_on;
}

void sbr_compare_init() {
    if (g_registered) return;
    // Either instrument needs the sink. The smoothness analyser needs EVERY present; the A/B does
    // not, and asking aurora for every frame when only the A/B is armed would cost a readback per
    // present for nothing.
    const bool wantSmooth = sbr_smooth_enabled();
    if (!sbr_compare_enabled() && !wantSmooth) return;
    g_registered = true;
    aurora_set_frame_sink(&on_aurora_frame, nullptr, wantSmooth ? 1 : g_every);
    if (sbr_compare_enabled())
        lucent::info("ab", "in-process A/B armed: scoring every {} presents against the aurora "
                           "oracle", g_every);
}

void sbr_compare_submit_native(const uint8_t* rgba, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    // A NEW baseline supersedes the previous frame's variants: they accumulate over every present
    // between aurora callbacks, and scoring them all against one aurora frame both inflates n and
    // compares a variant of one frame against a different frame's oracle.
    g_variants.clear();
    ++g_nativeSeq;
    if (!sbr_compare_enabled()) return;
    // Aurora's readback is asynchronous (copy at one present, map at the next), so the frame that
    // reaches the sink trails this one by a present or two. Keeping the LATEST native frame means
    // the pair can be up to ~2 frames apart; at 30 Hz with a mostly-static camera that is far
    // below the differences being measured, but it is a real skew and not pretended away.
    g_native.rgba.assign(rgba, rgba + (size_t)w * h * 4);
    g_native.w = w;
    g_native.h = h;
    g_native.valid = true;
    g_clear[0] = r; g_clear[1] = g; g_clear[2] = b;
}

bool sbr_compare_ablate_enabled() { return ablate(); }

void sbr_compare_submit_variant(int id, const char* name, const uint8_t* rgba, int w, int h) {
    if (!g_native.valid || rgba == nullptr) return;   // no baseline pending -> nothing to pair with
    Variant v;
    v.id = id;
    v.seq = g_nativeSeq;
    v.name = name != nullptr ? name : "?";
    v.w = w; v.h = h;
    v.rgba.assign(rgba, rgba + (size_t)w * h * 4);
    g_variants.push_back(std::move(v));
}

// THE ATTRIBUTION TABLE. Ranked by how much each ablation RECOVERS over the baseline, so the
// answer to "which operation is wrong" is the first row, with a number attached — not an
// inference drawn from two runs of different length.
void sbr_compare_report_attribution() {
    if (g_varAcc.empty()) return;
    double bi = 0, bc = 0;
    const double n = (double)g_scored.size();
    if (n <= 0) return;
    for (const auto& s : g_scored) { bi += s.iou; bc += s.corr; }
    bi /= n; bc /= n;
    struct Row { std::string name; double iou, corr, d; long n; };
    std::vector<Row> rows;
    for (const auto& [id, a] : g_varAcc) {
        if (a.n == 0) continue;
        const double mi = a.iou / (double)a.n, mc = a.corr / (double)a.n;
        rows.push_back({a.name, mi, mc, mi - bi, a.n});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.d > y.d; });
    if (g_variantDropped > 0)
        lucent::info("ab", "   ({} variant samples dropped: their baseline was consumed by an "
                           "aurora frame before the sweep finished)", g_variantDropped);
    lucent::info("ab", "OPERATION ATTRIBUTION — baseline edgeIoU {:.1f}% lumaCorr {:+.3f} over {} "
                       "frames. A POSITIVE delta means replacing that operation with a neutral "
                       "reference moved the frame TOWARD aurora, i.e. this port gets it wrong.",
                 bi, bc, (long)n);
    for (const Row& r : rows)
        lucent::info("ab", "   {:+6.1f}  {:<22} edgeIoU {:.1f}%  lumaCorr {:+.3f}  (n={})",
                     r.d, r.name, r.iou, r.corr, r.n);
}
