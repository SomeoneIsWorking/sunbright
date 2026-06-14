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
#include "../gx_parse.h"             // gxp_parse_frame, GxFrameInfo (per-object array-12 bases)
#include "../gx_stream.h"            // gxs_prev_frame_info — previous frame's parse
#include "Common/SunbrightHooks.h"   // sb_slot_xf_indexed — render-only matrix-interp seam

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unordered_set>
#include <unordered_map>

// 60 fps redraw window state (defined in interp_redraw.cpp).
extern bool g_interp60_in_redraw;

namespace {

// mode 3 registry: every J3DModel seen by the real field's viewCalc this frame.
// viewCalc runs once per model per frame (in the calc OR draw pass), so this is
// the full set of live models — and each one's mDrawMtxBuf double-buffer holds
// tick N-1 ([0][view]) and N ([1][view]) regardless of which pass computed it.
std::vector<u32> g_registry;
std::unordered_set<u32> g_blended_set;     // dedup blended buffers within one redraw
std::unordered_set<u32> g_blended_models;  // MODELS successfully blended this redraw (drawn interpolated)
std::unordered_set<u32> g_sdl_set;         // models registered via SDLModel::viewCalcSimple

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
        // Capture the worst offender this window — to identify WHY prev is garbage.
        if (moved > g_i60.g_worst) {
            g_i60.g_worst = moved;
            g_i60.g_model = model; g_i60.g_vt = mem_r32(model);
            g_i60.g_view = view; g_i60.g_n = n;
            g_i60.g_prevbuf = prev; g_i60.g_curbuf = cur;
            g_i60.g_prev0 = mem_rf32(prev + 3 * 4);   // joint 0 translation X
            g_i60.g_cur0  = mem_rf32(cur + 3 * 4);
            g_i60.g_src = g_sdl_set.count(model) ? 2 : 1;
        }
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
        // NaN guard only (uninitialized buffer). The magnitude/teleport threshold
        // is GONE — it misfired on distant geometry during camera rotation (large
        // but smooth eye-space deltas read as a "cut"); cuts are now detected at the
        // camera level (g_i60.is_cut) and skip the whole in-between.
        float dd = 0;
        for (u32 t = 3; t < 12; t += 4) {
            const float d = mem_rf32(cm + t * 4) - mem_rf32(pm + t * 4);
            dd += d * d;
        }
        if (!(dd == dd)) continue;                          // NaN: keep N
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
    g_blended_models.insert(model);   // this model is drawn interpolated → safe to skip its viewCalc
    g_i60.vc_blended++;
    return true;
}

}  // namespace

// True if `model` was successfully blended this redraw (its shape packets are prepared and its
// draw matrices hold the interpolated values). Only such models may have their viewCalc SKIPPED on
// the in-between; a model NOT blended (new this frame / single-buffered) must run viewCalc so
// prepareShapePackets() wires its shape packets — else J3DShape::draw reads a null draw-matrix.
bool interp60_model_blended(u32 model) { return g_blended_models.count(model) != 0; }

// ── Render-only pos-matrix interpolation (the new architecture) ──────────────────────────────────
// During the in-between REPLAY (gx_stream replay), the captured stream's indexed pos-matrix loads
// (CP array 12) read the model's mDrawMtxBuf[1][view] (= tick N). The fork's LoadIndexedXF hook
// (sb_slot_xf_indexed) calls sb_hook_xf_indexed below, which substitutes lerp(N-1, N): N-1 is
// mDrawMtxBuf[0][view], N is [1][view] — BOTH read-only from guest RAM; the interpolated result is
// written only into XF/GPU memory by LoadIndexedXF. No guest memory is modified, no game code runs.
// SMS double-buffers the draw matrices PER MODEL: each J3DModel/SDLModel owns mDrawMtxBuf[0] and
// [1]; after a real field's viewCalc, mDrawMtxBuf[1][view] holds tick N (the base the GX stream
// loads via CP array 12) and mDrawMtxBuf[0][view] holds tick N-1. We pair cur=[1][view] -> prev=
// [0][view] from the SAME registered model, so the mapping is by OBJECT IDENTITY. (The earlier
// draw-order prefix pairing — i-th array-12 base this frame ↔ i-th last frame — mispaired unrelated
// objects the instant the array-12 count shifted, e.g. a particle/object appearing or disappearing;
// lerping object A's matrix against object B's = exploded vertices. Identity pairing has no such
// failure: prev/cur always belong to the same object's own two buffers.) Guest RAM is only READ;
// the interpolated result is written only into XF/GPU memory by LoadIndexedXF.
static std::unordered_map<u32, u32> g_xfmap;   // cur pos-matrix base (phys) -> prev base (phys)
static std::unordered_set<u32> g_prev_registry; // models present LAST frame (have a valid N-1 buffer)
static bool  g_xf_active = false;
static float g_xf_alpha  = 0.5f;
// DBG_XF classification: every registered model's [1][v] (should be loaded by shapes) and [0][v]
// (the prev buffers) bases, so a miss can be tagged registered-[1] / registered-[0] / unknown.
static std::unordered_set<u32> g_dbg_d1, g_dbg_d0;

extern "C" void interp60_xfmap_build(const u8* frame, u32 n, float alpha) {
    g_xfmap.clear();
    g_xf_alpha = alpha;
    g_xf_active = true;
    g_i60.xf_hits = 0; g_i60.xf_misses = 0;
    const bool dbg = getenv("SUNBRIGHT_DBG_XF") != nullptr;
    if (dbg) { g_dbg_d1.clear(); g_dbg_d0.clear(); }
    std::unordered_set<u32> seen;   // this frame's models, to carry into g_prev_registry
    // Build the identity map from the model registry (every model the real field's viewCalc saw).
    for (u32 model : g_registry) {
        if (!ok_ram(model)) continue;
        seen.insert(model);
        if (dbg) {   // record ALL registered models' view bases (both buffers) for miss classification
            const u32 a0 = mem_r32(model + 0x60), a1 = mem_r32(model + 0x64);
            if (ok_ram(a0) && ok_ram(a1))
                for (u32 v = 0; v < 16; v++) {
                    const u32 c = mem_r32(a1 + 4 * v), p = mem_r32(a0 + 4 * v);
                    if (!ok_ram(c) || (c & 0x1F) || !ok_ram(p) || (p & 0x1F)) break;
                    g_dbg_d1.insert(c & 0x03FFFFFFu); g_dbg_d0.insert(p & 0x03FFFFFFu);
                }
        }
        // A model that wasn't drawn LAST frame has no tick-N-1 draw matrices: its mDrawMtxBuf[0]
        // is uninitialized (or holds a previous tenant's matrices from the heap). Interpolating
        // tick N against that garbage explodes the object for one field. Only interpolate models
        // present both frames — a freshly-spawned object simply renders at N (no interp) until its
        // second frame, when a real N-1 exists. (Not a magnitude threshold: a definitional gate —
        // no N-1 data exists, so there is nothing to interpolate.)
        if (!g_prev_registry.count(model)) continue;
        const u32 d0a = mem_r32(model + 0x60), d1a = mem_r32(model + 0x64);   // mDrawMtxBuf[0],[1] (Mtx**)
        if (!ok_ram(d0a) || !ok_ram(d1a)) continue;
        // SMS draws a model under several VIEWS (main camera, reflections, …) and double-buffers the
        // draw matrices: the two pointers a=[0][v], b=[1][v] are swapped every frame by swapDrawMtx,
        // so at any instant {a,b} = {this frame's buffer, last frame's buffer}. WHICH of the two the
        // GX stream actually loads has MIXED phase across objects (empirically ~90% load [0], the
        // rest [1]) — the number of swaps a model gets per frame varies (multi-pass / multi-view), so
        // it cannot be predicted from mDrawMtxBuf[1]/mCurrentViewNo at build time. So map BOTH
        // directions a<->b: whichever buffer the stream loads is the current frame, and the map
        // resolves to the OTHER buffer = the previous frame (lerp toward the loaded = current is
        // correct for any alpha). Both bases belong to the same model+view, so there is no
        // cross-object risk. Iterate all views; stop at the first non-heap-matrix slot (the Mtx**
        // array is contiguous 0..param_3-1, so this is exactly the view count, no OOB skip).
        for (u32 v = 0; v < 16; v++) {
            const u32 a = mem_r32(d0a + 4 * v), b = mem_r32(d1a + 4 * v);   // the two double-buffers
            if (!ok_ram(a) || (a & 0x1F)) break;           // end of the valid view array
            if (!ok_ram(b) || (b & 0x1F)) break;
            if (a == b) continue;                          // single-buffered for this view
            g_xfmap[b & 0x03FFFFFFu] = a & 0x03FFFFFFu;     // stream loaded [1] -> prev is [0]
            g_xfmap[a & 0x03FFFFFFu] = b & 0x03FFFFFFu;     // stream loaded [0] -> prev is [1]
        }
    }
    g_prev_registry.swap(seen);   // this frame's models become next frame's "present last frame"
    g_i60.xf_map_size = (unsigned long)g_xfmap.size();
    // SUNBRIGHT_DBG_XF: compare the identity-mapped objects against the array-12 bases the GX stream
    // actually sets this frame (registry coverage vs the GPU's real pos-matrix loads).
    if (getenv("SUNBRIGHT_DBG_XF")) {
        std::vector<u32> cur12;
        if (frame && n) {
            GxFrameInfo cur;
            gxp_parse_frame(frame, n, cur);
            for (const auto& a : cur.mtx_arrays) if (a.array == 12) cur12.push_back(a.base & 0x03FFFFFFu);
        }
        static int k = 0;
        if (k++ < 16)
            fprintf(stderr, "[xf] registry=%zu mapped=%zu gx_array12=%zu\n",
                    g_registry.size(), g_xfmap.size(), cur12.size());
    }
}
extern "C" void interp60_xfmap_end() { g_xf_active = false; g_xfmap.clear(); }

// Re-aim the SAME pairing map at a different blend alpha without rebuilding it (the map is a pure
// base<->base pairing; only the lerp weight changes). interp_redraw builds the map ONCE per game
// frame (with correct freshly-spawned gating off LAST frame's registry) and replays it twice: the
// REAL present at alpha 1.0 (== frame N) and the IN-BETWEEN at g_i60.alpha. At alpha 1.0 the lerp
// collapses to N for every model, so the real present is frame N regardless of pairing.
extern "C" void interp60_xfmap_set_alpha(float alpha) { g_xf_alpha = alpha; }

// Fork seam (XFStructs.cpp LoadIndexedXF). array 12 = XF_A pos matrix. Fill `out` (big-endian words,
// guest format — LoadIndexedXF swap32's them into XF) with the interpolated matrix; return true. A
// non-tracked base / inactive returns false (stock guest read).
// Per-array indexed-XF load histogram (diagnostic): which CP arrays does the scene load via
// indexed XF? 12 = position matrices (interpolated). Texture/texgen-projection matrices used by
// projected shadows/decals load via other arrays — if they appear here, the same seam can
// interpolate them. Read via /interp60.
unsigned long g_xf_array_hist[16] = {0};
extern "C" bool sb_hook_xf_indexed(u32 array, u32 base, u32 stride, u32 index, u32 size, u32* out) {
    if (g_xf_active && array < 16) g_xf_array_hist[array]++;
    if (!g_xf_active || array != 12) return false;
    static const int dbg = getenv("SUNBRIGHT_DBG_XF") ? 1 : 0;   // cached: hook is hot (per load)
    if (dbg) {   // first few hook bases — confirm parser base == load base
        static int k = 0;
        if (k++ < 8) fprintf(stderr, "[xf] hook array=%u base=%06x stride=%u index=%u size=%u\n",
                             array, base & 0x03FFFFFFu, stride, index, size);
    }
    auto it = g_xfmap.find(base & 0x03FFFFFFu);
    if (it == g_xfmap.end()) {                                       // not paired (static / new object)
        g_i60.xf_misses++;
        if (dbg) {   // classify distinct miss bases: registered-[1] / registered-[0] / unknown owner
            static std::unordered_set<u32> seen;
            const u32 b = base & 0x03FFFFFFu;
            if (seen.insert(b).second && seen.size() <= 40) {
                const char* cls = g_dbg_d1.count(b) ? "REG[1]-mapbug"
                                : g_dbg_d0.count(b) ? "REG[0]-phase"
                                : "UNKNOWN-owner";
                fprintf(stderr, "[xf-miss] base=%06x stride=%u size=%u  %s\n", b, stride, size, cls);
            }
        }
        return false;
    }
    g_i60.xf_hits++;
    const u32 cur_addr  = ((base & 0x03FFFFFFu) | 0x80000000u) + stride * index;   // guest tick N
    const u32 prev_addr = (it->second           | 0x80000000u) + stride * index;   // guest tick N-1
    const float a = g_xf_alpha;
    for (u32 i = 0; i < size; i++) {
        const u32 cw = mem_r32(cur_addr + i * 4), pw = mem_r32(prev_addr + i * 4);   // host-order
        float cf, pf; std::memcpy(&cf, &cw, 4); std::memcpy(&pf, &pw, 4);
        const float v = (1.0f - a) * pf + a * cf;
        u32 vw; std::memcpy(&vw, &v, 4);
        out[i] = __builtin_bswap32(vw);   // back to big-endian for LoadIndexedXF's swap32
    }
    return true;
}

// Install the fork slot at load (default-null otherwise).
static const bool s_xf_indexed_installed = (sb_slot_xf_indexed = &sb_hook_xf_indexed, true);

// Restore every blended model's mDrawMtxBuf[1][view] back to tick N, leaving the
// guest double-buffer exactly as the real field produced it. Called by
// interp_redraw.cpp after the in-between present.
void interp60_restore_after_redraw() {
    for (Saved& s : g_restore)
        for (size_t i = 0; i < s.f.size(); i++) mem_wf32(s.addr + (u32)i * 4, s.f[i]);
    g_restore.clear();
    g_blended_set.clear();
    g_blended_models.clear();
}

// mode 3: clear the registry at the start of a real frame (TMarDirector::direct).
void interp60_registry_clear() { g_registry.clear(); g_sdl_set.clear(); g_i60.g_worst = 0; }

// Number of live models the real field registered this frame (the scene's draw-matrix population).
// 0 / a sharply changing count = a menu/loading/transition scene, not a stable 3D scene to
// interpolate — interp_redraw uses this to skip the in-between across transitions.
size_t interp60_registry_size() { return g_registry.size(); }

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
        // mode 3 (registry buffer-blend): the registry was already blended before the re-issue.
        // Skip the body so the models that recompute in the draw pass don't overwrite the blend
        // back to N. We must NOT run viewCalc here: it mutates game memory (swapDrawMtx toggles
        // mCurrentViewNo, recompute + DCStoreRange) and that is not restored — interp60 is
        // render-only. A model whose shape packets aren't prepared (created this frame, never had a
        // viewCalc before the in-between) is handled READ-ONLY by ov_j3dshape_draw_guard below
        // (it skips that shape's draw instead of crashing on a null draw-matrix pointer).
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

// SDLModel::viewCalcSimple (0x8023d36c) — the STATIC WORLD/MAP draw-matrix computer.
// SDLModel : public J3DModel, so it shares the mDrawMtxBuf double-buffer layout and
// swapDrawMtx()s (draw[i] = camera × node[i] into [1][view]) exactly like J3DModel —
// blend_model handles it identically. The world judders because this is a SEPARATE
// class from J3DModel::viewCalc, so the registry missed it. Register it too.
extern "C" void func_8023d36c(CPUState&);
SUNBRIGHT_OVERRIDE(ov_sdl_viewCalcSimple, 0x8023d36cu) {
    if (g_interp60_in_redraw) {
        if (g_i60.mode == 3) return;   // registry pre-blended it; skip recompute (= frame N). No
                                       // viewCalc here (it would mutate mCurrentViewNo); unprepared
                                       // shapes are guarded read-only in ov_j3dshape_draw_guard.
        func_8023d36c(cpu);
        return;
    }
    const u32 model_this = cpu.gpr[3];
    func_8023d36c(cpu);
    if (g_i60.mode == 3 && model_this >= 0x80000000u) {
        g_i60.vc_realfield++;
        if (g_registry.size() < 4096) g_registry.push_back(model_this);
        g_sdl_set.insert(model_this);
    }
}

// J3DShape::draw read-only guard for the in-between (0x802e0390). A shape whose draw-matrix pointers
// aren't prepared — a model created THIS frame whose first viewCalc/prepareShapePackets is in the
// draw pass — has mDrawMatrices (shape+0x50) / mDrawMatrices[*mCurrentViewNo] null. On the in-between
// (viewCalc skipped to stay render-only) drawing it deref's that null (the file-select→gameplay
// mountStageArchive crash). We do NOT run viewCalc to prepare it (that mutates game memory:
// mCurrentViewNo swap, matrix recompute, DCStoreRange — interp60 must not touch game state). Instead
// just SKIP this shape's draw on the in-between; a brand-new object absent for one interpolated field
// is invisible, and the next real field draws it normally. Read-only: no game memory is modified.
extern "C" void func_802e0390(CPUState&);   // J3DShape::draw
SUNBRIGHT_OVERRIDE(ov_j3dshape_draw_guard, 0x802e0390u) {
    if (g_interp60_in_redraw) {
        const u32 shape = cpu.gpr[3];
        if (shape >= 0x80000000u) {
            const u32 dm  = mem_r32(shape + 0x50);   // mDrawMatrices (Mtx**)
            const u32 vno = mem_r32(shape + 0x58);   // mCurrentViewNo (u32*)
            if (dm < 0x80000000u || vno < 0x80000000u) return;             // pointers not prepared
            if (mem_r32(dm + mem_r32(vno) * 4) < 0x80000000u) return;      // null per-view matrices
        }
    }
    func_802e0390(cpu);
}

} // namespace
