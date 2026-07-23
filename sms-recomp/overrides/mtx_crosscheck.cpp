// mtx_crosscheck.cpp — check the native renderer's reconstruction against GROUND TRUTH, per draw.
//
// WHY THIS EXISTS. The native path was debugged for a whole session by scoring the FINAL COMPOSITED
// FRAME against aurora. A whole-frame number can say that something is wrong; it can never say
// WHERE, so every step became a hypothesis ("the sky writes depth", "a translucent overlay",
// "2D through the 3D projection", "skinned matrices") that cost a port to test and falsify. Worse,
// a REGRESSION survived: composing the view matrix a second time was committed with a confident
// justification because nothing measured that change on its own.
//
// The systematic replacement: the game TELLS the hardware which matrix each shape is drawn with.
// GXLoadPosMtxImm carries the exact position matrix, and PNMTXIDX selects it. Whatever the native
// renderer reconstructs from the J3D scene graph MUST equal that matrix. So instead of comparing
// pixels at the end of the pipeline, compare numbers at the point of use:
//
//     reconstructed drawable matrix  ==  the matrix the game loaded for that slot
//
// This is a differential test against ground truth, per draw, with a numeric delta. It localises to
// a shape and a matrix element rather than to "the frame looks wrong", and it would have caught the
// double view-compose the moment it was written: every matrix would have disagreed at once.
//
// The same shape generalises — a projection cross-check belongs here too when it lands.
//
//   SBR_MTX_CHECK=1   verify every drawable's matrix; report agreement + the worst offender

#include "overrides.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

extern "C" void func_80362e0c(CPUState&);   // GXLoadPosMtxImm(f32 mtx[3][4], u32 id)

namespace {

// GX matrix memory addressed in rows: PNMTX0=0, PNMTX1=3, ... so slot = id / 3, and the table is
// indexed by slot. 10 is GX's pos/nrm matrix count.
constexpr int kSlots = 64;

struct Loaded {
    float m[12];
    bool  valid = false;
};
Loaded g_loaded[kSlots];

struct Stats {
    unsigned long checked = 0;
    unsigned long agree = 0;
    unsigned long noRef = 0;      // nothing was loaded for that slot — cannot be checked
    float worst = 0.0f;
    u32   worstShape = 0;
    int   worstElem = 0;
    int   worstIdx = 0;
};
Stats g_st;

float guest_f32(u32 addr) {
    const u32 bits = sb_r32(addr);
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

void ov_gx_load_pos_mtx_imm(CPUState& cpu) {
    const u32 mtx = cpu.gpr[3];
    const u32 id  = cpu.gpr[4];
    const u32 slot = id / 3;
    if (slot < kSlots && sb_ram_fast(mtx) != nullptr) {
        for (int k = 0; k < 12; ++k) g_loaded[slot].m[k] = guest_f32(mtx + (u32)k * 4);
        g_loaded[slot].valid = true;
    }
    func_80362e0c(cpu);
}

} // namespace

bool sbr_mtx_check_enabled() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SBR_MTX_CHECK");
        v = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    return v == 1;
}

void sbr_mtx_crosscheck(u32 shape, int element, uint32_t slot, const float recon[12]) {
    if (!sbr_mtx_check_enabled()) return;
    if (slot >= kSlots || !g_loaded[slot].valid) { ++g_st.noRef; return; }

    // One-shot side-by-side of the first few disagreements. Deriving the relationship between what
    // J3D stores and what GX is loaded with needs the actual numbers, not another candidate guess.
    static int shown = 0;
    if (shown < 2) {
        ++shown;
        lucent::info("mtxcheck", "shape 0x{:08x} el {} slot {}", shape, element, slot);
        for (int r = 0; r < 3; ++r)
            lucent::info("mtxcheck", "  recon [{:9.2f} {:9.2f} {:9.2f} {:11.2f}]   gx [{:9.2f} "
                                     "{:9.2f} {:9.2f} {:11.2f}]",
                         recon[r * 4 + 0], recon[r * 4 + 1], recon[r * 4 + 2], recon[r * 4 + 3],
                         g_loaded[slot].m[r * 4 + 0], g_loaded[slot].m[r * 4 + 1],
                         g_loaded[slot].m[r * 4 + 2], g_loaded[slot].m[r * 4 + 3]);
    }

    ++g_st.checked;
    float worst = 0.0f;
    int worstIdx = 0;
    for (int k = 0; k < 12; ++k) {
        const float d = std::fabs(recon[k] - g_loaded[slot].m[k]);
        if (d > worst) { worst = d; worstIdx = k; }
    }
    // Tolerance is RELATIVE to the translation magnitude: the same absolute error is negligible on a
    // 6000-unit translation and fatal on a rotation element.
    const float scale = std::max(1.0f, std::fabs(g_loaded[slot].m[3]) + std::fabs(g_loaded[slot].m[7]) +
                                           std::fabs(g_loaded[slot].m[11]));
    if (worst <= 1e-3f * scale) ++g_st.agree;
    if (worst > g_st.worst) {
        g_st.worst = worst;
        g_st.worstShape = shape;
        g_st.worstElem = element;
        g_st.worstIdx = worstIdx;
    }
}

void sbr_mtx_report() {
    if (!sbr_mtx_check_enabled() || g_st.checked == 0) return;
    const double pct = 100.0 * (double)g_st.agree / (double)g_st.checked;
    // Loud when the reconstruction disagrees with what the game actually loaded: that is a defect in
    // the native path's transform, localised to a shape and a matrix element.
    if (g_st.agree == g_st.checked) {
        lucent::info("mtxcheck", "{} drawable matrices ALL match the loaded GX matrices "
                                 "({} unreferenced slots)", g_st.checked, g_st.noRef);
    } else {
        lucent::error("mtxcheck", "{:.1f}% of {} drawable matrices match GX ({} disagree); worst "
                                  "delta {:.3f} at element [{}] of shape 0x{:08x} el {}",
                      pct, g_st.checked, g_st.checked - g_st.agree, g_st.worst, g_st.worstIdx,
                      g_st.worstShape, g_st.worstElem);
    }
    g_st = Stats{};
}

SB_OVERRIDE(0x80362e0cu, ov_gx_load_pos_mtx_imm, "GXLoadPosMtxImm",
            "native render: record ground-truth position matrices (always runs the real body)")
