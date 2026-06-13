// 60 fps interpolation — J3DModel::viewCalc blend override (drives SUNBRIGHT_INTERP60).
//
// Pairs with interp_redraw.cpp. On the in-between field's draw pass
// (g_interp60_in_redraw == true) we substitute interpolated draw matrices so the
// shapes render half-way between tick N-1 and N.
//
// KEY (RE'd 2026-06-13 from reference/sms J3DModel::viewCalc @ 0x802deeb8):
// viewCalc does NOT merely load matrices to GX — it swapDrawMtx() then RECOMPUTES
// the per-joint draw matrices as drawMtx = viewMtx × nodeMatrix into
// mDrawMtxBuf[1][view] (J3DMTXConcatArrayIndexedSrc), and the shape packets later
// load mDrawMtxBuf[1][view] via an indexed CP array base (J3DShape::draw ->
// setModelDrawMtx). J3D double-buffers: after a real field's viewCalc, [0][view]
// holds tick N-1 and [1][view] holds tick N.
//
// So on the in-between field we must NOT run the guest body (it would swap +
// recompute = frame N again, destroying N-1 and overwriting any blend). Instead
// we read N-1 from [0][view] and N from [1][view], write the blend into [1][view]
// (what the shapes load), and restore [1][view]=N after the redraw present so the
// guest's double-buffer is left exactly as the real field made it.
//
// All controls/observations go through g_i60 (runtime/interp60.h) so the path is
// driveable live over /interp60 — no rebuild cycle.

#include "../overrides.h"
#include "../intrinsics.h"
#include "../interp60.h"

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <unordered_set>

// 60 fps redraw window state (defined in interp_redraw.cpp).
extern bool g_interp60_in_redraw;

namespace {

// mode 3 registry: every J3DModel seen by the real field's viewCalc this frame.
// viewCalc runs once per model per frame (in the calc OR draw pass), so this is
// the full set of live models — and each one's mDrawMtxBuf double-buffer holds
// tick N-1 ([0][view]) and N ([1][view]) regardless of which pass computed it.
std::vector<u32> g_registry;
std::unordered_set<u32> g_blended_set;   // dedup blended buffers within one redraw

// A translation jump beyond this distance between N-1 and N means a cut/respawn
// — draw tick N exactly rather than smear across the discontinuity.
constexpr float kSnapDist2 = 600.0f * 600.0f;

extern "C" void func_802deeb8(CPUState&);   // J3DModel::viewCalc

// Frame-N draw matrices saved before we overwrite [1][view] with the blend, so
// the redraw can be undone (restoring the guest double-buffer). Reused buffers.
struct Saved { u32 addr; std::vector<float> f; };
std::vector<Saved> g_restore;

// Per-window motion accumulator (reset each /interp60 report by interp_redraw).
double g_move_min = 1e30, g_move_max = 0, g_move_sum = 0; unsigned long g_move_n = 0;

// Produce the in-between draw matrices for one model into mDrawMtxBuf[1][view]
// (the buffer the shape packets load). Saves the N contents for
// interp60_restore_after_redraw(). No guest call, no buffer swap.
inline bool ok_ram(u32 a) { return a >= 0x80000000u && a < 0x81800000u; }

bool blend_model(u32 model) {
    if (!ok_ram(model)) { g_i60.vc_bail_null++; return false; }
    const u32 data = mem_r32(model + 0x04);
    const u32 view = mem_r32(model + 0x7C);
    if (!ok_ram(data) || view > 16) { g_i60.vc_bail_null++; return false; }
    const u32 n   = mem_r16(data + 0x98);                 // J3DDrawMtxData.mEntryNum
    const u32 d0a = mem_r32(model + 0x60), d1a = mem_r32(model + 0x64);
    if (!n || n > 512 || !ok_ram(d0a) || !ok_ram(d1a)) { g_i60.vc_bail_null++; return false; }
    const u32 prev = mem_r32(d0a + 4 * view);             // tick N-1
    const u32 cur  = mem_r32(d1a + 4 * view);             // tick N (shapes load this)
    // Matrix arrays are 0x20-aligned (new (0x20) Mtx[...]); reject anything that
    // isn't a plausible aligned heap matrix buffer — a bad write here corrupts a
    // pointer and faults later in the draw (RE'd: wild read of a float bit-pattern).
    if (!ok_ram(prev) || !ok_ram(cur) || (prev & 0x1F) || (cur & 0x1F)) { g_i60.vc_bail_null++; return false; }
    if (!ok_ram(cur + n * 48 - 1)) { g_i60.vc_bail_null++; return false; }   // whole array in RAM
    if (prev == cur)   { g_i60.vc_bail_single++; return false; } // single-buffered: no N-1
    if (!g_blended_set.insert(cur).second) return false;         // already blended this redraw

    // motion sample (pre-blend): mean squared translation delta over all joints.
    double moved = 0;
    for (u32 i = 0; i < n; i++)
        for (u32 t = 3; t < 12; t += 4) {
            const float d = mem_rf32(cur + i * 48 + t * 4) - mem_rf32(prev + i * 48 + t * 4);
            moved += (double)d * d;
        }
    moved /= n;
    if (moved == moved && moved < 1e30) {
        g_move_n++; g_move_sum += moved;
        if (moved < g_move_min) g_move_min = moved;
        if (moved > g_move_max) g_move_max = moved;
    }

    // Save N (the cur buffer) so the redraw can be undone after present.
    g_restore.push_back({});
    Saved& s = g_restore.back();
    s.addr = cur;
    s.f.resize((size_t)n * 12);
    for (u32 i = 0; i < n * 12; i++) s.f[i] = mem_rf32(cur + i * 4);

    // Record the buffer address so the redraw can cross-check it against the
    // pos-matrix array bases the GX stream actually sets (does the GPU read this?).
    if (g_i60.blended_addrs.size() < 4096) g_i60.blended_addrs.push_back(cur);

    const float a = g_i60.alpha;   // blend toward N
    for (u32 i = 0; i < n; i++) {
        const u32 pm = prev + i * 48, cm = cur + i * 48;   // Mtx = 3x4 f32
        if (g_i60.perturb) {                               // gross visible offset (A/B)
            for (u32 t = 3; t < 12; t += 4)
                mem_wf32(cm + t * 4, mem_rf32(cm + t * 4) + 300.0f);
            continue;
        }
        float dd = 0;
        for (u32 t = 3; t < 12; t += 4) {
            const float d = mem_rf32(cm + t * 4) - mem_rf32(pm + t * 4);
            dd += d * d;
        }
        if (dd > kSnapDist2 || !(dd == dd)) continue;      // jump/NaN: keep N
        for (u32 f = 0; f < 12; f++)
            mem_wf32(cm + f * 4, (1.0f - a) * mem_rf32(pm + f * 4) + a * mem_rf32(cm + f * 4));
    }

    // Track the FIRST blended model for the clobber check (re-read after all
    // draw lists) AND dump its model->packet->base wiring (the disconnect probe):
    // the GPU reads unk18[view]; it should equal cur. Set once per redraw.
    if (!g_i60.track_addr) {
        g_i60.track_addr = cur;
        g_i60.track_blended = mem_rf32(cur + 3 * 4);   // joint 0 translation X
        g_i60.track_model = model;
        g_i60.track_d1a = d1a;
        g_i60.track_cur = cur;
        g_i60.track_view = view;
        const u32 pkt = mem_r32(model + 0x84);          // mShapePackets[0]
        g_i60.track_pkt = pkt;
        if (pkt >= 0x80000000u) {
            const u32 unk18 = mem_r32(pkt + 0x18);      // pkt->unk18 = mDrawMtxBuf[1]
            g_i60.track_unk18 = unk18;
            g_i60.track_unk18_view = (unk18 >= 0x80000000u) ? mem_r32(unk18 + 4 * view) : 0;
        }
    }
    g_i60.vc_blended++;
    return true;
}

}  // namespace

// Restore every blended model's mDrawMtxBuf[1][view] back to tick N, leaving the
// guest double-buffer exactly as the real field produced it. Called by
// interp_redraw.cpp after the in-between present.
void interp60_restore_after_redraw() {
    for (Saved& s : g_restore)
        for (size_t i = 0; i < s.f.size(); i++) mem_wf32(s.addr + (u32)i * 4, s.f[i]);
    g_restore.clear();
    g_blended_set.clear();
}

// mode 3: clear the registry at the start of a real frame (TMarDirector::direct).
void interp60_registry_clear() { g_registry.clear(); }

// mode 3: blend every registered model's draw-matrix double-buffer toward N-1.
// Called on the in-between field BEFORE re-issuing the draw lists. Reaches the
// whole scene (every model that ran viewCalc this frame), not just the ~14 that
// recompute in the draw pass.
void interp60_blend_registry() {
    g_i60.reg_size = (unsigned long)g_registry.size();
    for (u32 model : g_registry)
        if (model >= 0x80000000u) blend_model(model);
}

// Publish + reset the per-window motion stats (called by the probe).
void interp60_take_motion() {
    g_i60.move_n   = g_move_n;
    g_i60.move_avg = g_move_n ? g_move_sum / g_move_n : 0.0;
    g_i60.move_min = g_move_n ? g_move_min : 0.0;
    g_i60.move_max = g_move_max;
    g_move_n = 0; g_move_sum = g_move_max = 0; g_move_min = 1e30;
}

namespace {

SUNBRIGHT_OVERRIDE(ov_j3d_viewCalc_blend, 0x802deeb8u) {
    if (g_interp60_in_redraw) {
        g_i60.vc_calls++;
        if (g_i60.cur_list >= 0 && g_i60.cur_list < 8) g_i60.vc_per_list[g_i60.cur_list]++;
        // mode 1 (buffer-blend): skip the guest body and post-blend the draw
        // matrices it would have produced. Only reaches models that run viewCalc.
        if (g_i60.mode == 1) {
            if (g_i60.blend && cpu.gpr[3] >= 0x80000000u) blend_model(cpu.gpr[3]);
            return;   // do NOT run the guest body (it would recompute = frame N)
        }
        // mode 3 (registry buffer-blend): the registry was already blended before
        // the re-issue. Skip the body so the ~14 models that recompute in the draw
        // pass don't overwrite the blend back to N.
        if (g_i60.mode == 3) return;
        // mode 2 (view-blend) / mode 0: run the guest body so it recomputes the
        // draw matrices against the (interpolated, in mode 2) j3dSys view matrix.
        func_802deeb8(cpu);
        return;
    }
    // Real field: compute draw matrices normally, and (mode 3) register the model.
    // Capture `this` (r3) BEFORE the call — func_802deeb8 clobbers r3.
    const u32 model_this = cpu.gpr[3];
    func_802deeb8(cpu);
    if (g_i60.mode == 3 && model_this >= 0x80000000u) {
        g_i60.vc_realfield++;
        if (g_registry.size() < 4096) g_registry.push_back(model_this);
    }
}

} // namespace
