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

SUNBRIGHT_OVERRIDE(ov_j3d_viewCalc_cap, 0x802deeb8u) {
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
