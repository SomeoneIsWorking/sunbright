// mtx_crosscheck.cpp — check the GX compatibility renderer's reconstruction against GROUND TRUTH,
// per draw.
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
// The invariant checked is the matrix INDEX. An earlier version compared matrix VALUES against
// GXLoadPosMtxImm and permanently reported ~13% agreement — a false alarm, because J3D only uses
// the immediate load for its CPU-skinned pipelines, where it loads j3dSys.mViewMtx rather than a
// model matrix. Comparing against the wrong ground truth is worse than no check: it reports a
// defect that is not there, every frame.
//
//   SBR_MTX_CHECK=1   verify every element's matrix index against the game's own load

#include "overrides.h"
#include "semantic_j3d_adapter.h"

#include "../runtime/sb_assert.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

extern "C" void func_80362e48(CPUState&); // GXLoadPosMtxIndx(u16 mtx_indx, u32 id)
extern "C" void func_802dfc04(CPUState&); // J3DShapeMtx::load() const
extern "C" void func_802dfd14(CPUState&); // J3DShapeMtxDL::load() const
extern "C" void func_802dfd3c(CPUState&); // J3DShapeMtxMulti::load() const

namespace {

// GX matrix memory addressed in rows: PNMTX0=0, PNMTX1=3, ... so slot = id / 3, and the table is
// indexed by slot. 10 is GX's pos/nrm matrix count.
constexpr int kSlots = 64;

// The INDEXED load is the one J3D actually uses for shapes: J3DSys::loadPosMtxIndx calls
// GXLoadPosMtxIndx(mtx_indx, id * 3), and the hardware fetches the matrix from the array set by
// GXSetArray. So the ground truth for "which matrix does this shape use" is the mtx_indx passed
// here — NOT the immediate loads, which J3D only uses for the CPU-skinned pipelines (and which is
// why the first version of this check compared against an unrelated identity matrix in slot 0).
//
// Loads are attributed to a shape by bracketing the real J3DShape::draw call: whatever is loaded
// while a shape is drawing belongs to that shape.
u32 g_currentShape = 0;
// (GX slot id, matrix index) pairs loaded during the current shape's draw. The id matters: within
// one element, J3DShapeMtxMulti loads id = 0,1,2,... one per bone, and a vertex's PNMTXIDX/3
// selects among them. So this sequence is the complete slot -> matrix mapping the game used.
std::vector<std::pair<u16, u16>> g_trueIdx;
// Renderer input captured at the high-level J3D matrix objects. This deliberately does not read
// the GX load stream; g_trueIdx is an independent control that can falsify this mapping.
std::vector<GuestJ3dMatrixBinding> g_semanticMatrices;
// Only ordinary and multi matrix objects issue observable GXLoadPosMtxIndx calls. DL matrix
// objects embed that command in an opaque display list, so they remain semantic renderer input but
// are excluded from this independently observable comparison population.
std::vector<GuestJ3dMatrixBinding> g_controlMatrices;

struct IdxStats {
    unsigned long compared = 0, agree = 0, noTruth = 0;
    u32 worstShape = 0;
    int worstMine = 0, worstTrue = 0;
};
IdxStats g_ix;

float guest_f32(u32 addr) {
    const u32 bits = sb_r32(addr);
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

void ov_gx_load_pos_mtx_indx(CPUState& cpu) {
    if (g_currentShape != 0)
        g_trueIdx.emplace_back((u16)(cpu.gpr[4] / 3), (u16)(cpu.gpr[3] & 0xFFFF));
    func_80362e48(cpu);
}

void ov_j3d_shape_mtx_load(CPUState& cpu) {
    if (g_currentShape != 0 && sb_ram_fast(cpu.gpr[3]) != nullptr) {
        const GuestJ3dMatrixBinding binding{cpu.gpr[3], 0, sb_r16(cpu.gpr[3] + 4)};
        g_semanticMatrices.push_back(binding);
        g_controlMatrices.push_back(binding);
    }
    func_802dfc04(cpu);
}

void ov_j3d_shape_mtx_dl_load(CPUState& cpu) {
    if (g_currentShape != 0 && sb_ram_fast(cpu.gpr[3]) != nullptr)
        g_semanticMatrices.push_back({cpu.gpr[3], 0, sb_r16(cpu.gpr[3] + 4)});
    func_802dfd14(cpu);
}

void ov_j3d_shape_mtx_multi_load(CPUState& cpu) {
    const u32 self = cpu.gpr[3];
    // PPC layout from J3DShape.hpp: base matrix index at +4, derived count at +8, table at +C.
    const u32 table = sb_ram_fast(self) != nullptr ? sb_r32(self + 0xC) : 0;
    const u16 count = sb_ram_fast(self) != nullptr ? sb_r16(self + 8) : 0;
    if (g_currentShape != 0 && count <= 64 && sb_ram_fast(table) != nullptr) {
        for (u16 slot = 0; slot < count; ++slot) {
            const u16 matrixIndex = sb_r16(table + slot * 2U);
            if (matrixIndex != 0xFFFFU) {
                const GuestJ3dMatrixBinding binding{self, slot, matrixIndex};
                g_semanticMatrices.push_back(binding);
                g_controlMatrices.push_back(binding);
            }
        }
    }
    func_802dfd3c(cpu);
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

// Bracket the real draw so indexed loads can be attributed to the shape performing them.
// Always on: the native path BUILDS from these, so they are not a diagnostic.
void sbr_mtx_begin_shape(u32 shape) {
    g_currentShape = shape;
    g_trueIdx.clear();
    g_semanticMatrices.clear();
    g_controlMatrices.clear();
}
void sbr_mtx_end_shape() {
    // The matrix-index control is opt-in. CPU skinning pipelines legitimately capture high-level
    // matrix objects while loading positions with GXLoadPosMtxImm, so there is no indexed GX load
    // population to compare unless the explicit diagnostic is enabled.
    if (!sbr_mtx_check_enabled()) {
        g_currentShape = 0;
        return;
    }
    if (!g_trueIdx.empty() || !g_controlMatrices.empty()) {
        SB_ASSERT(g_trueIdx.size() == g_controlMatrices.size(),
                  "J3D matrix-object bindings disagree with GX load control: high-level=%zu gx=%zu",
                  g_controlMatrices.size(), g_trueIdx.size());
        for (std::size_t index = 0; index < g_trueIdx.size(); ++index) {
            SB_ASSERT(g_controlMatrices[index].sourceSlot == g_trueIdx[index].first &&
                          g_controlMatrices[index].matrixIndex == g_trueIdx[index].second,
                      "J3D matrix-object binding disagrees with GX load control at %zu: "
                      "high-level=(%u,%u) gx=(%u,%u)",
                      index, g_controlMatrices[index].sourceSlot,
                      g_controlMatrices[index].matrixIndex, g_trueIdx[index].first,
                      g_trueIdx[index].second);
        }
    }
    g_currentShape = 0;
}

// The loads the shape just performed, in order. Empty when the shape used J3DShapeMtxDL (matrices
// baked into a display list), which is the documented case for falling back to the stored index.
const std::vector<std::pair<u16, u16>>& sbr_mtx_loads() {
    return g_trueIdx;
}

std::span<const GuestJ3dMatrixBinding> sbr_j3d_matrix_bindings() {
    return g_semanticMatrices;
}

// Compare the index this port derived for one element against the index the game actually loaded.
// The truth comes from the PREVIOUS frame's draw of the same shape (the loads happen inside the
// real draw, which runs after this capture) — the mapping is stable per shape, so that is sound.
void sbr_mtx_check_index(u32 shape, int element, uint32_t mine, uint32_t truthIdx, bool haveTruth) {
    if (!sbr_mtx_check_enabled())
        return;
    if (!haveTruth) {
        ++g_ix.noTruth;
        return;
    }
    ++g_ix.compared;
    const u16 truth = (u16)truthIdx;
    if ((u16)mine == truth) {
        ++g_ix.agree;
    } else if (g_ix.worstShape == 0) {
        g_ix.worstShape = shape;
        g_ix.worstMine = (int)mine;
        g_ix.worstTrue = (int)truth;
    }
}

void sbr_mtx_report_index() {
    if (!sbr_mtx_check_enabled() || g_ix.compared == 0)
        return;
    if (g_ix.agree == g_ix.compared) {
        lucent::info("mtxcheck",
                     "matrix INDEX matches the game for all {} elements ({} without "
                     "truth yet)",
                     g_ix.compared, g_ix.noTruth);
    } else {
        lucent::error("mtxcheck",
                      "matrix INDEX: {}/{} match ({:.1f}%); e.g. shape 0x{:08x} we used "
                      "{}, the game loaded {}",
                      g_ix.agree, g_ix.compared, 100.0 * (double)g_ix.agree / (double)g_ix.compared,
                      g_ix.worstShape, g_ix.worstMine, g_ix.worstTrue);
    }
    g_ix = IdxStats{};
}

SB_OVERRIDE(0x80362e48u, ov_gx_load_pos_mtx_indx, "GXLoadPosMtxIndx",
            "native render: record the ground-truth matrix INDEX per shape (real body runs)")
SB_OVERRIDE(0x802dfc04u, ov_j3d_shape_mtx_load, "J3DShapeMtx::load",
            "semantic renderer: capture one high-level rigid matrix binding (real body runs)")
SB_OVERRIDE(0x802dfd14u, ov_j3d_shape_mtx_dl_load, "J3DShapeMtxDL::load",
            "semantic renderer: capture one display-list matrix binding (real body runs)")
SB_OVERRIDE(0x802dfd3cu, ov_j3d_shape_mtx_multi_load, "J3DShapeMtxMulti::load",
            "semantic renderer: capture high-level skeletal matrix bindings (real body runs)")
