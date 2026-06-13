// 60 fps interpolation — J3DModel::viewCalc blend override (drives SUNBRIGHT_INTERP60).
//
// Pairs with interp_redraw.cpp: during the in-between field's draw pass
// (g_interp60_in_redraw == true), this override substitutes blended per-joint
// draw matrices instead of the real frame's, then runs the guest viewCalc body
// so the blend is what gets loaded to GX.
//
// J3DModel::viewCalc (0x802deeb8, GMSE01) is the function that LOADS the
// per-joint draw matrices to GX (the bl loop at +0xd4), from mDrawMatrices[1]
// [view] AFTER swapDrawMtx swaps [0]↔[1]. J3D double-buffers these: [0][view]
// holds frame N-1, [1][view] holds frame N. We write the midpoint into
// [0][view] (pre-swap) so that after viewCalc's swap, [1][view] == the blend.
// interp_redraw.cpp swaps the buffer pointers back after the pass.
//
// Heap addresses are stable for an object's lifetime, so the J3DModel* is a
// stable identity (see docs/model_interpolation.md).

#include "../overrides.h"
#include "../intrinsics.h"

#include <vector>

// 60 fps redraw window state (defined in interp_redraw.cpp, global scope).
extern bool g_interp60_in_redraw;
extern std::vector<u32> g_interp60_touched;

namespace {

// A translation jump beyond this distance between N-1 and N means a cut/respawn
// — draw tick N exactly rather than smear across the discontinuity.
constexpr float kSnapDist2 = 600.0f * 600.0f;

extern "C" void func_802deeb8(CPUState&);   // J3DModel::viewCalc

// Blend a model's draw/nrm matrices from tick N-1 toward N at t=1/2, IN PLACE
// into the [0][view] (N-1) buffer. After the guest viewCalc swaps [0]↔[1], the
// [1][view] buffer it loads to GX becomes this blend. Returns true if a blend
// was applied (the model is recorded for the pointer swap-back in interp_redraw).
bool interp60_blend_model(u32 model) {
    const u32 data = mem_r32(model + 0x04);
    const u32 view = mem_r32(model + 0x7C);
    const u32 n    = data ? mem_r16(data + 0x98) : 0;     // J3DDrawMtxData.mEntryNum
    const u32 d0a = mem_r32(model + 0x60), d1a = mem_r32(model + 0x64);
    const u32 n0a = mem_r32(model + 0x68), n1a = mem_r32(model + 0x6C);
    if (!n || n > 512 || !d0a || !d1a || !n0a || !n1a) return false;
    const u32 prev = mem_r32(d0a + 4 * view), cur = mem_r32(d1a + 4 * view);
    const u32 prevN = mem_r32(n0a + 4 * view), curN = mem_r32(n1a + 4 * view);
    if (!prev || !cur || !prevN || !curN) return false;
    if (prev == cur) return false;            // single-buffered: no N-1 source

    for (u32 i = 0; i < n; i++) {
        const u32 pm = prev + i * 48, cm = cur + i * 48;   // Mtx = 3x4 f32
        // translation delta (elements 3, 7, 11) — teleport/cut guard
        float dd = 0;
        for (u32 t = 3; t < 12; t += 4) {
            const float d = mem_rf32(cm + t * 4) - mem_rf32(pm + t * 4);
            dd += d * d;
        }
        if (dd > kSnapDist2 || !(dd == dd)) {              // jump or NaN: draw N
            for (u32 f = 0; f < 12; f++) mem_wf32(pm + f * 4, mem_rf32(cm + f * 4));
            const u32 pn = prevN + i * 36, cn = curN + i * 36;
            for (u32 f = 0; f < 9; f++) mem_wf32(pn + f * 4, mem_rf32(cn + f * 4));
            continue;
        }
        for (u32 f = 0; f < 12; f++) {
            const float a = mem_rf32(pm + f * 4), b = mem_rf32(cm + f * 4);
            mem_wf32(pm + f * 4, 0.5f * (a + b));
        }
        const u32 pn = prevN + i * 36, cn = curN + i * 36;
        for (u32 f = 0; f < 9; f++) {
            const float a = mem_rf32(pn + f * 4), b = mem_rf32(cn + f * 4);
            mem_wf32(pn + f * 4, 0.5f * (a + b));
        }
    }
    g_interp60_touched.push_back(model);
    return true;
}

SUNBRIGHT_OVERRIDE(ov_j3d_viewCalc_blend, 0x802deeb8u) {
    if (g_interp60_in_redraw && cpu.gpr[3] >= 0x80000000u)
        interp60_blend_model(cpu.gpr[3]);
    func_802deeb8(cpu);   // guest body: swaps [0]↔[1] and loads [1][view] to GX
}

} // namespace
