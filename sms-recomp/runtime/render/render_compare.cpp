// render_compare.cpp — in-process A/B of the native render against the aurora oracle.
// See render_compare.h for why this replaced the file-based harness.

#include "render_compare.h"

#include "native_render.h" // sbr_render_ablation_count/name — to name the variants NOT yet sampled
#include "render_compare_join.h"
#include "render_compare_metric.h"

#include "frame_smoothness.h"

#include <aurora/aurora.h>
#include <lucent/log.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

bool g_on = false;
bool g_registered = false;
bool g_sinkEveryPresent = false;
int g_every = 60;
long g_captures = 0;

// A MapAsync callback is expected within a handful of submissions, but the join must remain
// bounded even if the driver delays callbacks indefinitely. Capacity pressure is a loud refusal;
// it never evicts an older exact-key reservation and silently changes the comparison population.
sb::render_compare::FrameJoin g_join{64};

// Every scored frame that had geometry, so the report can be a distribution rather than a sample.
struct Score {
    double iou, corr;
};
std::vector<Score> g_scored;

// SBR_AB_SELFTEST=1 scores aurora against ITSELF. A metric that cannot report a perfect match on
// identical input is not measuring what it claims, and a bad score from it would be
// indistinguishable from a bad render — so the instrument is validated against a known-positive
// before its verdicts on the GX compatibility path are believed.
bool selftest() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SBR_AB_SELFTEST");
        v = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    return v == 1;
}

// ---- Operation attribution ----
// dIou/dCorr are PAIRED differences: variant-minus-baseline measured on the SAME frame against the
// SAME aurora capture, summed. The mean of those is drift-free by construction. The obvious
// alternative — mean(variant) - mean(baseline over every scored frame) — is not: variants are only
// scored on frames where the sweep completed and met its baseline, so the two means are taken over
// different frame sets, and this project's own rule is that aggregates at different sample counts
// are not comparable. That subtraction was here and is what this replaces.
struct VarAcc {
    std::string name;
    double iou = 0, corr = 0;
    double dIou = 0, dCorr = 0;
    long n = 0;
};
std::map<int, VarAcc> g_varAcc;
std::unique_ptr<sb::render_compare::AttributionControl> g_attributionControl;
std::atomic<int> g_ablNext{1}; // advanced only when a variant is actually scored
std::mutex g_scoreMutex;
sb::render_compare::IdentityControlResult g_metricIdentityControl =
    sb::render_compare::IdentityControlResult::Deferred;
bool g_metricDeferredReported = false;

bool ablate() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SBR_ABLATE");
        v = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    return v == 1;
}

void score_joined(sb::render_compare::JoinedFrame joined) {
    if (selftest() &&
        g_metricIdentityControl != sb::render_compare::IdentityControlResult::Passed) {
        return;
    }
    const auto& native = joined.baseline.image;
    const auto& oracle = joined.oracle;
    long lit = 0;
    const size_t npx = static_cast<size_t>(native.width) * native.height;
    for (size_t i = 0; i < npx; ++i) {
        const uint8_t* pixel = &native.rgba[i * 4];
        if (pixel[0] != joined.baseline.clear[0] || pixel[1] != joined.baseline.clear[1] ||
            pixel[2] != joined.baseline.clear[2]) {
            ++lit;
        }
    }

    const auto baselineScore =
        sb::render_compare::score_images(native.rgba.data(), native.width, native.height,
                                         oracle.rgba.data(), oracle.width, oracle.height);
    const double iou = baselineScore.edgeIou;
    const double corr = baselineScore.lumaCorrelation;

    bool reportAttribution = false;
    {
        std::scoped_lock lock{g_scoreMutex};
        ++g_captures;
        lucent::info("ab",
                     "#{} frame={} native {}x{} vs aurora {}x{} | geom {:.1f}% | edgeIoU "
                     "{:.1f}% | lumaCorr {:+.3f}",
                     g_captures, joined.frameId, native.width, native.height, oracle.width,
                     oracle.height, 100.0 * static_cast<double>(lit) / static_cast<double>(npx),
                     iou, corr);

        if (lit > 0) {
            g_scored.push_back({iou, corr});
            double sumIou = 0.0, sumCorr = 0.0, bestIou = 0.0;
            for (const auto& score : g_scored) {
                sumIou += score.iou;
                sumCorr += score.corr;
                bestIou = std::max(bestIou, score.iou);
            }
            const double count = static_cast<double>(g_scored.size());
            lucent::info("ab",
                         "    mean over {} exact-frame samples: edgeIoU {:.1f}% (best "
                         "{:.1f}%), lumaCorr {:+.3f}",
                         g_scored.size(), sumIou / count, bestIou, sumCorr / count);
            static const size_t kAt = [] {
                const char* value = std::getenv("SBR_AB_AT");
                return static_cast<size_t>(value != nullptr ? std::strtoul(value, nullptr, 10)
                                                            : 59);
            }();
            if (g_scored.size() == kAt) {
                lucent::info("ab",
                             "=== COMPARABLE @ N={}: edgeIoU {:.2f}% lumaCorr {:+.4f} "
                             "=== exact Aurora frame IDs (compare runs on THIS line)",
                             kAt, sumIou / count, sumCorr / count);
            }

            for (const auto& variant : joined.variants) {
                if (g_attributionControl != nullptr) {
                    g_attributionControl->observe(joined.baseline, variant);
                }
                const auto variantScore = sb::render_compare::score_images(
                    variant.image.rgba.data(), variant.image.width, variant.image.height,
                    oracle.rgba.data(), oracle.width, oracle.height);
                const double variantIou = variantScore.edgeIou;
                const double variantCorr = variantScore.lumaCorrelation;
                VarAcc& accumulator = g_varAcc[variant.id];
                accumulator.name = variant.name;
                accumulator.iou += variantIou;
                accumulator.corr += variantCorr;
                accumulator.dIou += variantIou - iou;
                accumulator.dCorr += variantCorr - corr;
                ++accumulator.n;
                const int ablationCount = sbr_render_ablation_count();
                if (ablationCount > 1) {
                    g_ablNext.store((variant.id % (ablationCount - 1)) + 1,
                                    std::memory_order_relaxed);
                }
            }
            reportAttribution =
                !g_varAcc.empty() && (g_scored.size() <= 5 || (g_scored.size() % 10) == 0);
        }
    }
    if (reportAttribution) {
        sbr_compare_report_attribution();
    }
}

void consume_ready(uint64_t frameId) {
    sb::render_compare::JoinedFrame joined;
    const auto status = g_join.take_ready(frameId, joined);
    if (status == sb::render_compare::JoinStatus::AwaitingPeer ||
        status == sb::render_compare::JoinStatus::UnknownFrame) {
        return;
    }
    if (status != sb::render_compare::JoinStatus::Accepted) {
        lucent::error("ab", "frame {} refused by exact join: {}", frameId,
                      sb::render_compare::join_status_name(status));
        return;
    }
    score_joined(std::move(joined));
}

void on_aurora_frame(const uint8_t* rgba, uint32_t w, uint32_t h, const AuroraFrameSinkInfo* info,
                     void*) {
    // Smoothness is a property of CONSECUTIVE presents, so it sees every informed sink delivery.
    // A/B reservations below are independently sparse and exact-keyed.
    sbr_smooth_feed(rgba, static_cast<int>(w), static_cast<int>(h));
    if (!sbr_compare_enabled())
        return;
    if (info == nullptr || info->structSize < sizeof(AuroraFrameSinkInfo) ||
        info->version != AURORA_FRAME_SINK_INFO_VERSION || info->frameId == 0) {
        lucent::error("ab", "frame sink refused invalid identity metadata");
        return;
    }

    // Metric known-positive control. Unlike the retired self-test, this validates only the metric;
    // it does not overwrite the native side inside the callback and bypass the frame join.
    if (selftest() &&
        g_metricIdentityControl == sb::render_compare::IdentityControlResult::Deferred) {
        const auto identityScore =
            sb::render_compare::score_images(rgba, static_cast<int>(w), static_cast<int>(h), rgba,
                                             static_cast<int>(w), static_cast<int>(h));
        const auto result = sb::render_compare::evaluate_identity_control(identityScore);
        if (result == sb::render_compare::IdentityControlResult::Deferred) {
            if (!g_metricDeferredReported) {
                g_metricDeferredReported = true;
                lucent::info("ab",
                             "metric self-test deferred: identical frame {} has no comparable "
                             "edge population or luma variance",
                             info->frameId);
            }
        } else {
            g_metricIdentityControl = result;
            if (result == sb::render_compare::IdentityControlResult::Passed) {
                lucent::info("ab",
                             "metric self-test passed on non-degenerate identical frame: "
                             "edgeIoU {:.1f}% lumaCorr {:+.3f} (frame {})",
                             identityScore.edgeIou, identityScore.lumaCorrelation, info->frameId);
            } else {
                lucent::error("ab",
                              "metric self-test FAILED on non-degenerate identical frame: "
                              "edgeIoU {:.1f}% lumaCorr {:+.3f} (frame {}); A/B verdicts "
                              "suppressed",
                              identityScore.edgeIou, identityScore.lumaCorrelation, info->frameId);
            }
        }
    }

    const auto status =
        g_join.submit_oracle(info->frameId, rgba, static_cast<int>(w), static_cast<int>(h));
    if (status == sb::render_compare::JoinStatus::UnknownFrame) {
        return; // expected when smoothness forces sink cadence 1 but A/B is sparse
    }
    if (status != sb::render_compare::JoinStatus::Accepted) {
        lucent::error("ab", "Aurora oracle for frame {} refused: {}", info->frameId,
                      sb::render_compare::join_status_name(status));
        return;
    }
    consume_ready(info->frameId);
}

} // namespace

bool sbr_compare_enabled() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SBR_AB");
        v = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
        if (const char* n = std::getenv("SBR_AB_EVERY"))
            g_every = std::max(1, std::atoi(n));
        g_on = v == 1;
    }
    return g_on;
}

void sbr_compare_init() {
    if (g_registered)
        return;
    // Either instrument needs the sink. The smoothness analyser needs EVERY present; the A/B does
    // not, and asking aurora for every frame when only the A/B is armed would cost a readback per
    // present for nothing.
    const bool wantSmooth = sbr_smooth_enabled();
    if (!sbr_compare_enabled() && !wantSmooth)
        return;
    g_registered = true;
    g_sinkEveryPresent = wantSmooth;
    aurora_set_frame_sink_with_info(&on_aurora_frame, nullptr, wantSmooth ? 1 : g_every);
    if (sbr_compare_enabled()) {
        if (ablate()) {
            int controlId = -1;
            for (int id = 1; id < sbr_render_ablation_count(); ++id) {
                if (std::strcmp(sbr_render_ablation_name(id), "control:no-op") == 0) {
                    controlId = id;
                    break;
                }
            }
            if (controlId < 0) {
                lucent::error("ab", "attribution disabled: no control:no-op ablation exists");
            } else {
                g_attributionControl =
                    std::make_unique<sb::render_compare::AttributionControl>(controlId);
            }
        }
        lucent::info("ab",
                     "in-process A/B armed: scoring every {} presents against the aurora "
                     "oracle by exact frame ID",
                     g_every);
    }
}

uint64_t sbr_compare_capture_frame_id() {
    if (!sbr_compare_enabled())
        return 0;
    const uint64_t frameId = aurora_frame_sink_capture_frame_id();
    if (frameId == 0)
        return 0;
    // Smoothness forces Aurora to capture every present. Keep A/B sparse on its own caller cadence;
    // callbacks for the intervening unreserved IDs feed smoothness and are ignored by the join.
    if (g_sinkEveryPresent && g_every > 1) {
        static uint64_t captureCall = 0;
        if ((captureCall++ % static_cast<uint64_t>(g_every)) != 0)
            return 0;
    }
    const auto status = g_join.reserve(frameId);
    if (status != sb::render_compare::JoinStatus::Accepted) {
        lucent::error("ab", "could not reserve Aurora frame {}: {} ({} pending)", frameId,
                      sb::render_compare::join_status_name(status), g_join.pending());
        return 0;
    }
    return frameId;
}

void sbr_compare_capture_current_native_frame() {
    if (!sbr_compare_enabled())
        return;
    const uint64_t frameId = sbr_compare_capture_frame_id();
    if (frameId == 0)
        return;

    static std::vector<uint8_t> pixels(640 * 448 * 4);
    if (sbr_render_capture() && sbr_render_readback(pixels.data(), 640, 448)) {
        sbr_compare_submit_native(frameId, pixels.data(), 640, 448, 26, 102, 204);
        if (sbr_compare_ablate_enabled()) {
            // The control renders the real pipeline and must reproduce this exact-frame baseline.
            // The historical all-variants burst hung the graphics ring, so one round-robin variant
            // is rendered per selected frame.
            const auto checksum = [](const std::vector<uint8_t>& image) {
                unsigned long long hash = 1469598103934665603ULL;
                for (size_t index = 0; index < image.size(); index += 4) {
                    hash = (hash ^ (image[index] + 3u * image[index + 1] + 7u * image[index + 2])) *
                           1099511628211ULL;
                }
                return hash;
            };
            const auto dump = [](const char* path, const std::vector<uint8_t>& image) {
                if (FILE* file = std::fopen(path, "wb")) {
                    std::fwrite(image.data(), 1, image.size(), file);
                    std::fclose(file);
                }
            };
            const unsigned long long baselineChecksum = checksum(pixels);
            static long reported = 0;
            const int ablationCount = sbr_render_ablation_count();
            const int ablation = sbr_compare_ablation_to_render();
            const bool report = sbr_render_last_vertex_count() > 1000 &&
                                reported < static_cast<long>(ablationCount) * 2;
            if (report && reported == 0)
                dump("scratch/bin/sweep_baseline.rgba", pixels);
            if (ablation > 0 && ablation < ablationCount && sbr_render_ablation_render(ablation) &&
                sbr_render_readback(pixels.data(), 640, 448)) {
                if (report && ablation == 9)
                    dump("scratch/bin/sweep_pinunit1.rgba", pixels);
                if (report) {
                    ++reported;
                    const unsigned long long variantChecksum = checksum(pixels);
                    lucent::info("ab", "   sweep checksum: baseline {:016x}  {} {:016x}{}",
                                 baselineChecksum, sbr_render_ablation_name(ablation),
                                 variantChecksum,
                                 variantChecksum == baselineChecksum ? "  (identical)" : "");
                }
                sbr_compare_submit_variant(frameId, ablation, sbr_render_ablation_name(ablation),
                                           pixels.data(), 640, 448);
            }
        }
    }
    sbr_compare_finish_frame(frameId);
}

void sbr_compare_submit_native(uint64_t frameId, const uint8_t* rgba, int w, int h, uint8_t r,
                               uint8_t g, uint8_t b) {
    if (!sbr_compare_enabled())
        return;
    const auto status = g_join.submit_baseline(frameId, rgba, w, h, r, g, b);
    if (status != sb::render_compare::JoinStatus::Accepted) {
        lucent::error("ab", "native baseline for Aurora frame {} refused: {}", frameId,
                      sb::render_compare::join_status_name(status));
    }
}

bool sbr_compare_ablate_enabled() {
    return ablate();
}

void sbr_compare_submit_variant(uint64_t frameId, int id, const char* name, const uint8_t* rgba,
                                int w, int h) {
    const auto status = g_join.submit_variant(frameId, id, name, rgba, w, h);
    if (status != sb::render_compare::JoinStatus::Accepted) {
        lucent::error("ab", "variant {} for Aurora frame {} refused: {}",
                      name != nullptr ? name : "?", frameId,
                      sb::render_compare::join_status_name(status));
    }
}

void sbr_compare_finish_frame(uint64_t frameId) {
    const auto status = g_join.seal(frameId);
    if (status != sb::render_compare::JoinStatus::Accepted) {
        lucent::error("ab", "could not seal Aurora frame {}: {}", frameId,
                      sb::render_compare::join_status_name(status));
        return;
    }
    consume_ready(frameId);
}

// THE ATTRIBUTION TABLE. Ranked by how much each ablation RECOVERS over the baseline, so the
// answer to "which operation is wrong" is the first row, with a number attached — not an
// inference drawn from two runs of different length.
int sbr_compare_ablation_to_render() {
    return g_ablNext.load(std::memory_order_relaxed);
}

void sbr_compare_report_attribution() {
    std::scoped_lock lock{g_scoreMutex};
    if (g_varAcc.empty())
        return;
    if (g_attributionControl == nullptr || !g_attributionControl->table_allowed()) {
        static bool reportedWaiting = false;
        static bool reportedFailure = false;
        if (g_attributionControl != nullptr &&
            g_attributionControl->state() == sb::render_compare::ControlState::Failed) {
            if (!reportedFailure) {
                reportedFailure = true;
                lucent::error("ab", "OPERATION ATTRIBUTION SUPPRESSED: control:no-op did not "
                                    "byte-match its exact-frame baseline");
            }
        } else if (!reportedWaiting) {
            reportedWaiting = true;
            lucent::info("ab", "operation attribution withheld until control:no-op byte-matches "
                               "its exact-frame baseline");
        }
        return;
    }
    double bi = 0, bc = 0;
    const double n = (double)g_scored.size();
    if (n <= 0)
        return;
    for (const auto& s : g_scored) {
        bi += s.iou;
        bc += s.corr;
    }
    bi /= n;
    bc /= n;
    struct Row {
        std::string name;
        double iou, corr, d, dc;
        long n;
    };
    std::vector<Row> rows;
    for (const auto& [id, a] : g_varAcc) {
        if (a.n == 0)
            continue;
        const double mi = a.iou / (double)a.n, mc = a.corr / (double)a.n;
        rows.push_back({a.name, mi, mc, a.dIou / (double)a.n, a.dCorr / (double)a.n, a.n});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.d > y.d; });
    lucent::info("ab",
                 "OPERATION ATTRIBUTION — baseline edgeIoU {:.1f}% lumaCorr {:+.3f} over {} "
                 "scored frames. Each delta is PAIRED (variant minus baseline on one exact "
                 "frame and Aurora capture). Different rows still sample different round-robin "
                 "scene frames, so this is exploratory until their frame populations match.",
                 bi, bc, (long)n);
    int flat = 0;
    for (const Row& r : rows) {
        if (r.d == 0.0 && r.dc == 0.0)
            ++flat;
        lucent::info("ab",
                     "   {:+6.1f}  {:<22} edgeIoU {:.1f}%  lumaCorr {:+.3f} (d {:+.3f})  "
                     "(n={}){}",
                     r.d, r.name, r.iou, r.corr, r.dc, r.n,
                     (r.d == 0.0 && r.dc == 0.0) ? "  <- IDENTICAL to baseline" : "");
    }
    // A row at exactly zero is not "this operation is already right". It is far more often "this
    // operation is not exercised by the frame at all" — the plaza frames measured on 2026-08-12
    // bind no texture unit above 0, so the seven per-unit pins CANNOT move, and reading their 0.0
    // as evidence about unit routing would be reading the absence of an input as a result.
    // WHAT THIS TABLE DOES NOT COVER. The sweep is round-robin (one variant per scored frame), so
    // an early table is genuinely partial — and a partial table with no statement of what is
    // missing reads exactly like a complete one. Name the absentees.
    {
        std::string missing;
        int nmiss = 0;
        for (int a = 1; a < sbr_render_ablation_count(); ++a)
            if (g_varAcc.find(a) == g_varAcc.end() || g_varAcc[a].n == 0) {
                if (!missing.empty())
                    missing += ", ";
                missing += sbr_render_ablation_name(a);
                ++nmiss;
            }
        if (nmiss > 0)
            lucent::info("ab",
                         "   NOT YET SAMPLED ({} of {}): {} — this table is partial, not a "
                         "finished ranking.",
                         nmiss, sbr_render_ablation_count() - 1, missing);
    }
    if (flat > 0)
        lucent::info("ab",
                     "   ({} of {} ablations scored EXACTLY 0.0 on both metrics. That is "
                     "the signature of an operation the frame never performs, not of one "
                     "this port already gets right — check that the frame exercises it "
                     "before drawing any conclusion from the row.",
                     flat, (int)rows.size());
}
