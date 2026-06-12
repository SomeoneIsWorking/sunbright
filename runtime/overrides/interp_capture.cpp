// 60fps interpolation — stage 1: per-model joint-matrix capture (SUNBRIGHT_INTERP=1).
//
// Hooks J3DModel::viewCalc (0x802deeb8, GMSE01) — runs once per model per drawn frame,
// after calc() filled mNodeMatrices (+0x58: per-joint 3x4 world matrices, model space;
// joint count at mModelData(+0x04)+0x1C). Snapshots every model's matrices into a
// host-side double buffer keyed by the J3DModel* (heap addresses are stable for an
// object's lifetime → pointer = identity; see docs/model_interpolation.md).
//
// This is the data foundation for synthesized in-between frames: prev[id] + cur[id]
// per joint → slerp/lerp at fraction t, replay the draw. Stage 1 only captures and
// reports (models/frame, joints, ID churn) so the replay stage has measured reality
// to build against.

#include "../overrides.h"
#include "../intrinsics.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

unsigned long long watchdog_vi_fields();   // watchdog.cpp — frame epoch source

// 60 fps redraw window state (defined in interp_redraw.cpp, global scope).
extern bool g_interp60_in_redraw;
extern std::vector<u32> g_interp60_touched;

namespace {

struct ModelCap {
    std::vector<float> prev, cur;          // jointNum * 12 floats each
    unsigned long long prev_epoch = 0, cur_epoch = 0;
    u32 joints = 0;
};

std::unordered_map<u32, ModelCap> g_models;
unsigned long long g_last_report = 0;
unsigned long g_snaps = 0, g_new_ids = 0;

extern "C" void func_802deeb8(CPUState&);

// 60 fps redraw window (interp_redraw.cpp): while the in-between frame's draw
// pass runs, viewCalc must NOT recompute (same tick = same matrices) and must
// NOT run the guest body (its swapDrawMtx would break buffer parity). Instead:
// blend buf0 (tick N-1) toward buf1 (tick N) at t=1/2, written into buf0 in
// place (buf0 is dead until the next real swap), then swap the buf0/buf1
// pointers so the shape packets — which index mDrawMtxBuf[1][viewNo] at draw —
// fetch the blend. interp_redraw swaps back after the pass. Per-matrix
// teleport guard: a translation jump beyond kSnapDist means a cut/respawn —
// draw tick N exactly rather than smear across it.
constexpr float kSnapDist2 = 600.0f * 600.0f;

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

    for (u32 i = 0; i < n; i++) {
        const u32 pm = prev + i * 48, cm = cur + i * 48;   // Mtx = 3x4 f32
        // translation delta (elements 3, 7, 11)
        float dd = 0;
        for (u32 t = 3; t < 12; t += 4) {
            const float d = mem_rf32(cm + t * 4) - mem_rf32(pm + t * 4);
            dd += d * d;
        }
        if (dd > kSnapDist2) {
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
    // expose the blend as buf1 (what the shape packets index at draw)
    mem_w32(d0a + 4 * view, cur);  mem_w32(d1a + 4 * view, prev);
    mem_w32(n0a + 4 * view, curN); mem_w32(n1a + 4 * view, prevN);
    g_interp60_touched.push_back(model);
    return true;
}

SUNBRIGHT_OVERRIDE(ov_j3d_viewCalc_cap, 0x802deeb8u) {
    if (g_interp60_in_redraw) {
        if (cpu.gpr[3] >= 0x80000000u) interp60_blend_model(cpu.gpr[3]);
        return;     // never run the guest body inside the redraw window
    }
    static const bool on = getenv("SUNBRIGHT_INTERP") != nullptr;
    const u32 model = cpu.gpr[3];
    if (on && model >= 0x80000000u) {
        const u32 data   = mem_r32(model + 0x04);
        const u32 mtxs   = mem_r32(model + 0x58);
        const u32 joints = data ? mem_r16(data + 0x1C) : 0;
        if (mtxs >= 0x80000000u && joints > 0 && joints < 512) {
            const unsigned long long epoch = watchdog_vi_fields();
            ModelCap& mc = g_models[model];
            if (mc.cur.empty()) g_new_ids++;
            if (mc.cur_epoch != epoch) {            // first sight this frame: rotate buffers
                mc.prev.swap(mc.cur);
                mc.prev_epoch = mc.cur_epoch;
                mc.cur_epoch  = epoch;
            }
            mc.joints = joints;
            mc.cur.resize((size_t)joints * 12);
            for (u32 j = 0; j < joints * 12; j++)
                mc.cur[j] = mem_rf32(mtxs + j * 4);
            g_snaps++;

            if (epoch - g_last_report >= 600) {     // ~10 s of fields
                size_t total_joints = 0, live = 0;
                for (auto& [k, v] : g_models)
                    if (epoch - v.cur_epoch < 4) { live++; total_joints += v.joints; }
                fprintf(stderr,
                        "[interp] field=%llu live_models=%zu joints=%zu snaps=%lu new_ids=%lu tracked=%zu\n",
                        epoch, live, total_joints, g_snaps, g_new_ids, g_models.size());
                g_last_report = epoch;
                g_snaps = g_new_ids = 0;
                // expire models unseen for ~5 s so the map doesn't grow unbounded
                for (auto it = g_models.begin(); it != g_models.end();)
                    it = (epoch - it->second.cur_epoch > 300) ? g_models.erase(it) : std::next(it);
            }
        }
    }
    func_802deeb8(cpu);
}

} // namespace
