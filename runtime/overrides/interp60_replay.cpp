// 60fps interpolation — RENDER-ONLY replay architecture (Zelda64Recomp / Twilight-Princess-PC style).
//
// THE MODEL (user-specified): interp60 lives ON TOP of the game and never modifies game memory:
//   1. Frame N draws  → COLLECT the drawn objects' transforms (read-only snapshot into our memory).
//   2. Hold, wait for frame N+1.
//   3. Frame N+1 draws → collect.
//   4. Present:  frame N  →  interp(N, N+1)  →  frame N+1     (one frame of latency).
// The in-between is rendered by REPLAYING frame N's captured GX command stream (we own it:
// gx_stream.cpp g_frame / g_prev_frame) with the transform matrices interpolated — NOT by re-running
// the game's draw code (that re-execution is what corrupted state / hit unprepared shapes / stepped
// particles in the old in-place-blend design). Nothing in guest memory is written.
//
// WHY THE OLD DESIGN WAS WRONG (and is being replaced): the old path mutated the game's
// mDrawMtxBuf double-buffer in place (blend toward N-1) and RE-ISSUED the game's draw perform-lists,
// then restored. That (a) modifies game memory — interp60 must be render-only — and (b) re-executes
// game logic, which faulted on freshly-created models, stepped JPA particles, and corrupted the
// TApplication boot/director state machine on macOS. Replay avoids both.
//
// MATRIX PAIRING (the one hard problem, solved): SMS draws with INDEXED pos-matrix arrays (GX CP
// array 12). The matrix VALUES live in J3DModel::mDrawMtxBuf[1][view]; the GX stream references them
// by the array BASE address. The game double-buffers across the view index: frame N's draw uses
// view = mCurrentViewNo, frame N-1 used view = 1 - mCurrentViewNo, and BOTH arrays are still resident
// in mDrawMtxBuf[1] at frame N. So for each registered model we read both (read-only), lerp them into
// a SCRATCH guest buffer we own, and on the replay rewrite that model's GXSetArray(array 12) base to
// the scratch. The matrices the GPU reads are interpolated; game memory is untouched.
//
// STATUS: scaffolding for the new architecture, gated OFF by default (SUNBRIGHT_INTERP60_REPLAY=1)
// so the current path is unaffected while this is built and verified. Implemented here: the
// read-only per-model transform snapshot + interpolation into scratch. TODO (next): the GX-stream
// replay with base-redirection (rewrite GXSetArray array-12 bases in a copy of g_frame, feed to the
// OpcodeDecoder) and the one-frame-latency present pipeline (present N, interp, N+1).
#include "../overrides.h"
#include "../intrinsics.h"
#include "../interp60.h"
#include <vector>
#include <cstdlib>

#ifdef HAVE_DOLPHIN_CORE

// Opt-in while under construction; default OFF leaves the existing interp60 path in charge.
bool sunbright_interp60_replay() {
    static int v = -1;
    if (v < 0) v = getenv("SUNBRIGHT_INTERP60_REPLAY") ? 1 : 0;
    return v == 1;
}

namespace {

// J3DModel field offsets (RE'd; see interp_capture.cpp blend_model).
constexpr u32 OFF_DATA      = 0x04;   // J3DModelData*
constexpr u32 OFF_DRAWBUF0  = 0x60;   // mDrawMtxBuf[0] base (Mtx**)
constexpr u32 OFF_DRAWBUF1  = 0x64;   // mDrawMtxBuf[1] base (Mtx**)
constexpr u32 OFF_VIEWNO    = 0x7C;   // mCurrentViewNo (u32)
constexpr u32 MTX_BYTES     = 48;     // one Mtx = 3x4 f32

inline bool ok_ram(u32 a) { return a >= 0x80000000u && a < 0x81800000u; }

// One model's transform snapshot for a frame: the array base it draws from and the raw matrix bytes.
struct ModelXform {
    u32 model = 0;
    u32 base  = 0;          // &mDrawMtxBuf[1][view][0] — the GXSetArray base this frame
    u32 count = 0;          // number of Mtx in the array
    std::vector<u8> bytes;  // count * MTX_BYTES, copied READ-ONLY from game memory
};

// Snapshot every registered model's current-view draw-matrix array, read-only. The caller supplies
// the model list (interp_capture's registry). This is the "collect drawn objects" step.
std::vector<ModelXform> g_snap_cur, g_snap_prev;

// Read the draw-matrix count for a model from its J3DModelData (getDrawMtxNum). Conservative bound.
u32 model_draw_mtx_count(u32 model) {
    const u32 data = MEM_R32(model + OFF_DATA);
    if (!ok_ram(data)) return 0;
    // J3DModelData::getDrawMtxNum() reads mDrawMtxData.mDrawMtxNum; offset verified against the
    // count used by viewCalc's DCStoreRange(getDrawMtxPtr(), getDrawMtxNum()*sizeof(Mtx)).
    const u32 n = MEM_R16(data + 0x10);   // mJointTree/.. mDrawMtxNum (see J3DModelData layout)
    return (n <= 1024) ? n : 0;
}

}  // namespace

// "Collect drawn objects" for the current frame: snapshot each model's current-view matrix array.
// Read-only — copies bytes OUT of game memory; writes nothing back. `models` = interp_capture registry.
extern "C" void sb_interp60_collect(const u32* models, unsigned n) {
    if (!sunbright_interp60_replay()) return;
    g_snap_prev.swap(g_snap_cur);    // last frame's snapshot becomes "previous"
    g_snap_cur.clear();
    for (unsigned i = 0; i < n; i++) {
        const u32 model = models[i];
        if (!ok_ram(model)) continue;
        const u32 buf1 = MEM_R32(model + OFF_DRAWBUF1);
        const u32 view = MEM_R32(model + OFF_VIEWNO);
        if (!ok_ram(buf1) || view > 1) continue;
        const u32 base = MEM_R32(buf1 + view * 4);   // mDrawMtxBuf[1][view]
        const u32 cnt  = model_draw_mtx_count(model);
        if (!ok_ram(base) || cnt == 0) continue;
        ModelXform mx; mx.model = model; mx.base = base; mx.count = cnt;
        mx.bytes.resize(cnt * MTX_BYTES);
        for (u32 b = 0; b < cnt * MTX_BYTES; b++) mx.bytes[b] = MEM_R8(base + b);
        g_snap_cur.push_back(std::move(mx));
    }
}

// Interpolate a model's matrices between the previous and current snapshots into `out` (a host
// buffer of count*MTX_BYTES floats, big-endian as the GPU reads). alpha 0 = prev, 1 = cur. Returns
// false if the model isn't paired (new this frame) — caller draws it un-interpolated (at N).
extern "C" bool sb_interp60_interp_model(u32 model, float alpha, u8* out, u32 out_cap) {
    if (!sunbright_interp60_replay()) return false;
    const ModelXform* cur = nullptr; const ModelXform* prev = nullptr;
    for (const auto& m : g_snap_cur)  if (m.model == model) { cur  = &m; break; }
    for (const auto& m : g_snap_prev) if (m.model == model) { prev = &m; break; }
    if (!cur || !prev || cur->count != prev->count) return false;
    const u32 n = cur->count * (MTX_BYTES / 4);
    if (n * 4 > out_cap) return false;
    for (u32 i = 0; i < n; i++) {
        const u32 ci = ((u32)cur->bytes[i*4] << 24) | ((u32)cur->bytes[i*4+1] << 16) |
                       ((u32)cur->bytes[i*4+2] << 8) | cur->bytes[i*4+3];
        const u32 pi = ((u32)prev->bytes[i*4] << 24) | ((u32)prev->bytes[i*4+1] << 16) |
                       ((u32)prev->bytes[i*4+2] << 8) | prev->bytes[i*4+3];
        float cf, pf; __builtin_memcpy(&cf, &ci, 4); __builtin_memcpy(&pf, &pi, 4);
        const float v = (1.0f - alpha) * pf + alpha * cf;
        u32 vi; __builtin_memcpy(&vi, &v, 4);
        out[i*4] = (u8)(vi >> 24); out[i*4+1] = (u8)(vi >> 16);
        out[i*4+2] = (u8)(vi >> 8); out[i*4+3] = (u8)vi;
    }
    return true;
}

#endif
