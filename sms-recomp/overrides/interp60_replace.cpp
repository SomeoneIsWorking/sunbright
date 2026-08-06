// interp60_replace.cpp — RECORD-AND-REPLACE frame interpolation, after dusklight.
//
// WHY THIS EXISTS. The sub-frame this file replaces works by SUBSTITUTION: write an interpolated
// pose into the guest's own objects, re-run parts of the game from it, render, then put everything
// back. Every seam that path touches has produced a leak that had to be chased separately —
// the camera's cached matrix, j3dSys's global view, the player's waist smoothers, 340,509 pixels of
// actor state, and most recently a PreEntry re-issue at cue 0x4 that also runs requestShadow() and
// floods the shadow manager (decomp src/Strategic/liveactor.cpp:408). Each fix was correct and each
// exposed the next one, because the approach's failure mode is unbounded: anything the re-issued
// code writes outlives the sub-frame unless someone enumerated it.
//
// dusklight (TwilitRealm, a shipping TP port on the same decomp+Aurora architecture, CC0) does not
// have this problem, and not because it enumerated better. Its model — src/dusk/frame_interpolation.
// {h,cpp} — is RECORD-AND-REPLACE: the sim tick runs UNTOUCHED, every final matrix is recorded, the
// presentation frame lerps prev->cur into a replacement table, and draw-time sites read the
// replacement instead of the real matrix. Guest state is never written, so it cannot leak. That is a
// structural property, not a diligence property, which is why it is worth adopting wholesale.
//
// WHAT IS DIFFERENT HERE, AND WHY. dusklight resolves matrices through lookup_replacement() at about
// twenty hand-edited call sites (d_drawlist, d_a_midna, d_flower, d_grass...). It can: it owns decomp
// source for the code that draws. We are recompiling retail PPC and cannot edit a draw site. So the
// replacement is applied by TEMPORARILY OVERWRITING the live draw-matrix buffer and restoring it
// byte-exactly afterwards. The window is the sub-frame's draw lists and nothing else; no game code
// that computes anything runs inside it.
//
// That is still a write to guest memory, so it is worth being precise about why it is not the same
// hazard as substitution. Substitution writes an INPUT (a pose) and then runs game code that derives
// state from it — the derived state is what leaks, and it is unbounded because deriving is what game
// code does. This writes an OUTPUT (the final matrix a shape is drawn with), consumed only by the
// draw, and restores it before any game code runs again. The leak surface is one buffer per model
// with a known size, and restore() checks it.
//
// THE KEY IS (model, index), NOT THE MATRIX ADDRESS. dusklight keys most matrices by their own
// address and uses a composite getInterpKey(model, n) where it needs to disambiguate. Here the
// composite key is mandatory: J3DModel::viewCalc BEGINS with swapDrawMtx(), so a given joint's
// matrix lives at one of two addresses on alternate ticks (J3DModel.hpp:294, mDrawMtxBuf[2]). Keying
// by address would make prev and cur never match and the interpolation would silently do nothing —
// which, note, is a failure that produces a perfectly plausible-looking frame.
//
// WHAT THIS DOES NOT COVER. A control proves the fields that ARE wired; it says nothing about the
// ones that are not. Recorded and interpolated: J3DModel draw matrices (Mtx, 3x4) and normal
// matrices (Mtx33). NOT covered, and therefore still stepping at the tick rate:
//   - the PROJECTION matrix, so a changing fovy (zoom, cutscene lens) will not interpolate;
//   - texture and bump matrices (mBumpMtxArr), so scrolling/env-mapped UVs step;
//   - anything not drawn through a J3DModel — J2D/ortho HUD, JPA particles, immediate-mode geometry;
//   - a model whose joint count changes between ticks (counted as `recount` and skipped).
// The report line names these, because "0 models drifted" must not read as "everything is covered".
//
// THE NO-OP CONTROL. SBR_INTERP60_REPLACE_ALPHA=1.0 writes `cur` into the live buffer — the values
// already there — so the sub-frame must come out byte-identical to one rendered with replacement
// off. That is a control that MUST score exactly 0; if it does not, the write path is wrong
// independently of anything the lerp does. Run it before believing any reading from this file.
//
//   SBR_INTERP60_REPLACE=1        use this path instead of substitute-and-re-issue
//   SBR_INTERP60_REPLACE_ALPHA=x  pin alpha (the no-op control is 1.0)
//   SBR_INTERP60_REPLACE_NONRM=1  interpolate draw matrices only, leaving normals at the tick rate
//                                 (an ablation: it isolates lighting lag from geometry lag)

#include "interp60_replace.h"

#include "overrides.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

// J3DModel (decomp include/JSystem/J3D/J3DGraphAnimator/J3DModel.hpp:284).
constexpr u32 OFF_JM_MODELDATA = 0x04;
constexpr u32 OFF_JM_DRAWBUF1  = 0x64;   // mDrawMtxBuf[1]
constexpr u32 OFF_JM_NRMBUF1   = 0x6C;   // mNrmMtxBuf[1]
constexpr u32 OFF_JM_VIEWNO    = 0x7C;   // mCurrentViewNo
// J3DModelData::mDrawMtxData is at +0x98 and its first field is u16 mEntryNum
// (J3DVertex.hpp:123), i.e. getDrawMtxNum().
constexpr u32 OFF_JMD_DRAWMTXNUM = 0x98;

constexpr int kMtxFloats   = 12;   // Mtx   3x4
constexpr int kMtx33Floats = 9;    // Mtx33 3x3

// A model with more draw matrices than this is not a model — it is a bad pointer or a stale object,
// and reading 100 MB from it would look like a hang rather than a fault. The largest real SMS
// skeletons are a few hundred joints.
constexpr u32 kMaxDrawMtx = 1024;

struct Rec {
    u32 n = 0;
    std::vector<float> draw;   // n * 12
    std::vector<float> nrm;    // n * 9
    bool haveNrm = false;
    u32  stamp = 0;            // the tick this entry was last written in
};

struct Saved {
    u32 model = 0;
    u32 drawPtr = 0, nrmPtr = 0;
    u32 n = 0;
    std::vector<u32> drawWords;   // raw guest words, for a byte-exact restore
    std::vector<u32> nrmWords;
    bool haveNrm = false;
    // FNV-1a over the words this file WROTE. restore() re-hashes the live buffer against it, which
    // is how "something else recomputed these matrices mid-sub-frame" becomes visible instead of
    // being silently overwritten by the restore.
    uint64_t wroteHash = 0;
};

constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime  = 1099511628211ull;

inline void fnv_word(uint64_t& h, u32 w) {
    for (int b = 0; b < 4; ++b) { h ^= (u8)(w >> (8 * b)); h *= kFnvPrime; }
}

std::unordered_map<u32, Rec> g_cur, g_prev;
std::vector<Saved>           g_saved;

u32  g_tick = 0;
bool g_applied = false;

// Every counter here is a DENOMINATOR for something the report says. A bare "interpolated 0 models"
// is indistinguishable from "recorded nothing", "matched nothing" and "was never called".
struct Stats {
    unsigned long ticks = 0, recorded = 0, subframes = 0;
    unsigned long matched = 0, unmatched = 0, recount = 0, badPtr = 0, absurdN = 0;
    unsigned long moved = 0;          // models whose prev and cur actually differ
    unsigned long clobbered = 0;      // buffers something else wrote during the sub-frame
    unsigned long restoredMtx = 0;
    // A DISTRIBUTION, not a mean. The mean of the per-tick translation displacement read 1.4e34,
    // because one model (0x81391d1c) carries finite-but-absurd garbage in its matrices and a single
    // 1.7e37 outlier swamps twenty million samples. Buckets cannot be swamped, and the shape is the
    // answer anyway: the question is whether a TYPICAL drawn matrix moves a tick's worth, and a
    // histogram says that where a mean cannot.
    unsigned long bucket[7] = {0};     // 0 | <0.01 | <1 | <10 | <100 | <1e4 | >=1e4
    unsigned long transN = 0;
    unsigned long nonFinite = 0;
    float maxDelta = 0.0f;
    u32   maxModel = 0;
} g_st;

float alpha_override(float alpha) {
    static const char* e = std::getenv("SBR_INTERP60_REPLACE_ALPHA");
    if (!e || !*e) return alpha;
    return (float)std::atof(e);
}

// A constant added to every replaced matrix's translation. 0 (the default) is the real path.
float kick() {
    static const float v = [] {
        const char* e = std::getenv("SBR_INTERP60_REPLACE_KICK");
        return e && *e ? (float)std::atof(e) : 0.0f;
    }();
    return v;
}

bool nrm_disabled() {
    static const bool v = std::getenv("SBR_INTERP60_REPLACE_NONRM") != nullptr;
    return v;
}

float guest_f32(u32 addr) {
    const u32 bits = sb_r32(addr);
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

void guest_w_f32(u32 addr, float v) {
    u32 bits;
    std::memcpy(&bits, &v, sizeof bits);
    sb_w32(addr, bits);
}

// mDrawMtxBuf[1][mCurrentViewNo] — the Mtx* the game hands a packet (J3DModel.hpp:45).
u32 buf1_ptr(u32 model, u32 offBuf1) {
    const u32 basePtr = sb_r32(model + offBuf1);
    if (!sb_ram_fast(basePtr)) return 0;
    const u32 viewNo = sb_r32(model + OFF_JM_VIEWNO);
    if (viewNo > 4) return 0;
    const u32 ea = basePtr + viewNo * 4;
    if (!sb_ram_fast(ea)) return 0;
    const u32 p = sb_r32(ea);
    return sb_ram_fast(p) ? p : 0;
}

// getDrawMtxNum(), or 0 when the model cannot be read. 0 is refused by every caller rather than
// treated as "an empty model".
u32 draw_mtx_num(u32 model) {
    if (!sb_ram_fast(model + OFF_JM_MODELDATA)) return 0;
    const u32 md = sb_r32(model + OFF_JM_MODELDATA);
    if (!sb_ram_fast(md + OFF_JMD_DRAWMTXNUM)) return 0;
    const u32 n = sb_r16(md + OFF_JMD_DRAWMTXNUM);
    if (n == 0) return 0;
    if (n > kMaxDrawMtx) {
        ++g_st.absurdN;
        static bool said = false;
        if (!said) {
            said = true;
            lucent::error("i60r",
                          "model 0x{:08x} reports {} draw matrices (cap {}). Refusing to record it; "
                          "this is a bad J3DModel pointer or a layout drift, NOT a large model.",
                          model, n, kMaxDrawMtx);
        }
        return 0;
    }
    return n;
}

} // namespace

bool sbr_i60r_enabled() {
    static const bool v = std::getenv("SBR_INTERP60_REPLACE") != nullptr;
    return v;
}

void sbr_i60r_begin_tick() {
    if (!sbr_i60r_enabled()) return;
    ++g_tick;
    ++g_st.ticks;
    g_prev.swap(g_cur);
    // Entries are cleared rather than the map, so the per-model vectors keep their capacity and a
    // steady-state tick allocates nothing.
    for (auto& kv : g_cur) kv.second.n = 0;
}

void sbr_i60r_record(u32 model) {
    if (!sbr_i60r_enabled() || !sb_ram_fast(model)) return;

    const u32 n = draw_mtx_num(model);
    if (n == 0) return;
    const u32 drawPtr = buf1_ptr(model, OFF_JM_DRAWBUF1);
    if (!drawPtr || !sb_ram_fast(drawPtr + n * kMtxFloats * 4 - 4)) { ++g_st.badPtr; return; }

    Rec& r = g_cur[model];
    r.n = n;
    r.stamp = g_tick;
    r.draw.resize((size_t)n * kMtxFloats);
    for (u32 i = 0; i < n * kMtxFloats; ++i) r.draw[i] = guest_f32(drawPtr + i * 4);

    const u32 nrmPtr = nrm_disabled() ? 0 : buf1_ptr(model, OFF_JM_NRMBUF1);
    r.haveNrm = nrmPtr && sb_ram_fast(nrmPtr + n * kMtx33Floats * 4 - 4);
    if (r.haveNrm) {
        r.nrm.resize((size_t)n * kMtx33Floats);
        for (u32 i = 0; i < n * kMtx33Floats; ++i) r.nrm[i] = guest_f32(nrmPtr + i * 4);
    }
    ++g_st.recorded;
}

int sbr_i60r_apply(float alpha) {
    if (!sbr_i60r_enabled()) return 0;
    g_saved.clear();
    g_applied = true;
    ++g_st.subframes;

    const float a = alpha_override(alpha);

    for (auto& kv : g_cur) {
        const u32  model = kv.first;
        const Rec& cur   = kv.second;
        if (cur.n == 0 || cur.stamp != g_tick) continue;   // not recorded THIS tick

        auto it = g_prev.find(model);
        if (it == g_prev.end() || it->second.n == 0) { ++g_st.unmatched; continue; }
        const Rec& prev = it->second;
        if (prev.n != cur.n) { ++g_st.recount; continue; }   // model rebuilt; no correspondence

        const u32 n = cur.n;
        const u32 drawPtr = buf1_ptr(model, OFF_JM_DRAWBUF1);
        if (!drawPtr || !sb_ram_fast(drawPtr + n * kMtxFloats * 4 - 4)) { ++g_st.badPtr; continue; }
        const bool doNrm = cur.haveNrm && prev.haveNrm;
        const u32  nrmPtr = doNrm ? buf1_ptr(model, OFF_JM_NRMBUF1) : 0;
        const bool okNrm  = nrmPtr && sb_ram_fast(nrmPtr + n * kMtx33Floats * 4 - 4);

        Saved s;
        s.model = model;
        s.drawPtr = drawPtr;
        s.nrmPtr = okNrm ? nrmPtr : 0;
        s.n = n;
        s.haveNrm = okNrm;

        const u32 dWords = n * kMtxFloats;
        s.drawWords.resize(dWords);
        for (u32 i = 0; i < dWords; ++i) s.drawWords[i] = sb_r32(drawPtr + i * 4);

        uint64_t h = kFnvOffset;
        bool movedThis = false;
        for (u32 i = 0; i < dWords; ++i) {
            const float p = prev.draw[i], c = cur.draw[i];
            const float d = c - p;
            if (d != 0.0f) movedThis = true;
            // The DISPLACEMENT statistic, over translation elements only.
            //
            // A max is the wrong summary here: it reported 1.7e37 for the whole run, which is a
            // non-finite/garbage entry in some model's unused matrix slot, and it hid the number
            // that matters — how far a typical drawn matrix moves in one tick. That number is the
            // denominator for "alpha changed few pixels": if prev and cur are a tick apart under a
            // camera swinging 49 units/tick, translations must move on that order, and if they do
            // not then the recorded pair is not a tick apart and no lerp of it can be right.
            if ((i % kMtxFloats) == 3 || (i % kMtxFloats) == 7 || (i % kMtxFloats) == 11) {
                const float ad = d < 0 ? -d : d;
                if (!(ad <= 3.4e38f)) {
                    ++g_st.nonFinite;
                } else {
                    ++g_st.transN;
                    const int b = ad == 0.0f      ? 0
                                  : ad < 0.01f    ? 1
                                  : ad < 1.0f     ? 2
                                  : ad < 10.0f    ? 3
                                  : ad < 100.0f   ? 4
                                  : ad < 10000.0f ? 5
                                                  : 6;
                    ++g_st.bucket[b];
                    if (ad > g_st.maxDelta) { g_st.maxDelta = ad; g_st.maxModel = model; }
                }
            }
            // SBR_INTERP60_REPLACE_KICK=<units> — A CONTROL THAT MUST FIRE.
            //
            // "The replacement is connected but moves few pixels" and "the replacement writes a
            // buffer nothing draws from" produce the same small number, and the second is the more
            // likely of the two given how much of a J3D frame is drawn from display lists. So:
            // displace every replaced matrix by a constant and LOOK. If the scene does not visibly
            // come apart, mDrawMtxBuf is not what the hardware reads and every reading taken from
            // this path is measuring the wrong buffer.
            //
            // Column 3 of each row is the translation (Mtx is row-major 3x4), so element index
            // i % 12 == 3, 7, 11.
            const bool isTranslation = (i % kMtxFloats) == 3 || (i % kMtxFloats) == 7 ||
                                       (i % kMtxFloats) == 11;
            guest_w_f32(drawPtr + i * 4, p + d * a + (isTranslation ? kick() : 0.0f));
            fnv_word(h, sb_r32(drawPtr + i * 4));
        }

        if (okNrm) {
            const u32 nWords = n * kMtx33Floats;
            s.nrmWords.resize(nWords);
            for (u32 i = 0; i < nWords; ++i) s.nrmWords[i] = sb_r32(nrmPtr + i * 4);
            for (u32 i = 0; i < nWords; ++i) {
                const float p = prev.nrm[i];
                guest_w_f32(nrmPtr + i * 4, p + (cur.nrm[i] - p) * a);
                fnv_word(h, sb_r32(nrmPtr + i * 4));
            }
        }
        s.wroteHash = h;

        g_st.restoredMtx += n;
        if (movedThis) ++g_st.moved;
        ++g_st.matched;
        g_saved.push_back(std::move(s));
    }
    return (int)g_saved.size();
}

void sbr_i60r_restore() {
    if (!sbr_i60r_enabled() || !g_applied) return;
    g_applied = false;

    for (const Saved& s : g_saved) {
        // Did anything WRITE these matrices while the sub-frame was drawing? If so the sub-frame is
        // running code that recomputes model-view matrices (a viewCalc, a skin deform), the
        // replacement was partly discarded, and the restore below is papering over it. This is the
        // check that would have caught the whole substitute-and-re-issue class of bug early, so it
        // is not optional and it is not gated.
        //
        // Comparing against the values WE wrote — not against the saved originals, which would
        // report a difference on every model by construction.
        uint64_t h = kFnvOffset;
        for (u32 i = 0; i < (u32)s.drawWords.size(); ++i) fnv_word(h, sb_r32(s.drawPtr + i * 4));
        if (s.haveNrm)
            for (u32 i = 0; i < (u32)s.nrmWords.size(); ++i) fnv_word(h, sb_r32(s.nrmPtr + i * 4));
        if (h != s.wroteHash) {
            ++g_st.clobbered;
            static unsigned long said = 0;
            if (++said <= 4)
                lucent::error("i60r",
                              "model 0x{:08x}: its draw matrices CHANGED during the sub-frame's "
                              "draw lists (wrote {:016x}, found {:016x}). Something in the re-issue "
                              "recomputes model-view matrices, so the interpolated values were "
                              "partly discarded and this restore is hiding it.",
                              s.model, s.wroteHash, h);
        }
        for (u32 i = 0; i < (u32)s.drawWords.size(); ++i) sb_w32(s.drawPtr + i * 4, s.drawWords[i]);
        if (s.haveNrm)
            for (u32 i = 0; i < (u32)s.nrmWords.size(); ++i) sb_w32(s.nrmPtr + i * 4, s.nrmWords[i]);
    }
    g_saved.clear();
}

void sbr_i60r_report() {
    if (!sbr_i60r_enabled()) return;
    static unsigned long last = 0;
    if (g_st.subframes - last < 300) return;
    const unsigned long window = g_st.subframes - last;
    last = g_st.subframes;

    lucent::info("i60r",
                 "record-and-replace: {} sub-frames, {} models recorded over {} ticks | "
                 "matched {} (of which {} actually MOVED), unmatched {}, joint-count changed {}, "
                 "bad pointer {}, CLOBBERED mid-draw {} | per-tick translation displacement over {} "
                 "elements ({} non-finite, EXCLUDED) IN THE LAST {} SUB-FRAMES ONLY (the buckets "
                 "reset every report, because a cumulative histogram averages a parked camera "
                 "together with a moving one and cannot answer which): zero {} | <0.01 {} | <1 {} "
                 "| <10 {} | <100 {} "
                 "| <1e4 {} | >=1e4 {} (max {:.3g} on model 0x{:08x}){}",
                 g_st.subframes, g_st.recorded, g_st.ticks, g_st.matched, g_st.moved,
                 g_st.unmatched, g_st.recount, g_st.badPtr, g_st.clobbered,
                 g_st.transN, g_st.nonFinite, window,
                 g_st.bucket[0], g_st.bucket[1], g_st.bucket[2], g_st.bucket[3], g_st.bucket[4],
                 g_st.bucket[5], g_st.bucket[6], (double)g_st.maxDelta, g_st.maxModel,
                 g_st.matched == 0
                     ? "   <-- NOTHING was replaced. Either no model was recorded in two consecutive "
                       "ticks, or the recorder never ran; alpha cannot change a pixel either way."
                     : (g_st.moved == 0
                            ? "   <-- matched but NOTHING moved: prev == cur for every model, so "
                              "alpha cannot change a pixel and a 0 reading here means the scene is "
                              "static, not that interpolation works."
                            : ""));
    lucent::info("i60r",
                 "  NOT covered by this path (still stepping at the tick rate): projection matrix "
                 "(fovy/zoom), texture and bump matrices, J2D/ortho HUD, JPA particles, "
                 "immediate-mode geometry.{}",
                 nrm_disabled() ? " Normal matrices ablated OFF by SBR_INTERP60_REPLACE_NONRM." : "");
    for (int i = 0; i < 7; ++i) g_st.bucket[i] = 0;
    g_st.transN = 0;
    g_st.nonFinite = 0;
}
