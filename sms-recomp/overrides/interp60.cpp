// interp60.cpp — 60 fps output from a 30 Hz simulation, by rendering the frame twice.
//
// The game simulates at 30 Hz. Rather than run it faster (which changes physics, timers and every
// piece of logic keyed on the retrace count), the RENDER is decoupled: each game tick is presented
// twice, and the second present shows the scene half-way between the previous tick and this one.
//
// ARCHITECTURE — replay, not re-simulation. Re-running the game's draw code for the in-between
// would double-step animation and crash; instead the captured GX stream is replayed a second time
// (dev_gxfifo builds it once, the frame seam sends it twice). The two matrix paths interpolate
// differently, and BOTH are needed or the scene only half-moves:
//
//   * INDEXED matrices (GXLoadPosMtxIndx, envelope-skinned parts like Mario): the stream references
//     them through host pointers into guest RAM, so blending the guest mDrawMtxBuf before the replay
//     is enough — aurora re-reads the blended values. That is what this file does.
//   * IMMEDIATE matrices (GXLoadPosMtxImm, the RIGID world and camera — ~half of every frame): their
//     VALUES are baked inline into the stream, so a guest-RAM blend never touches them. Those are
//     blended in the STREAM by dev_gxfifo (gxfifo_blend_last), pairing each immediate matrix with the
//     previous frame's positionally. Skipping this left the world and camera frozen while only Mario
//     interpolated — "looks like vanilla framerate" (2026-07-23).
//
// ORDER: the in-between (N-½) is presented BEFORE the real tick (N). N-½ is temporally earlier, so
// presenting N first would judder. The frame seam sends the blended stream, presents, restores the
// guest RAM, then sends and presents N.
//
// WHERE THE TWO TICKS LIVE (RE'd from J3DModel::viewCalc @ 0x802deeb8, per the retired
// interp_capture.cpp): viewCalc does not merely load matrices — it swaps the double buffer and
// recomputes each joint's draw matrix as view x node into mDrawMtxBuf[1][view], which is what the
// shape packets then load. So after a real field: [0][view] holds tick N-1 and [1][view] holds
// tick N. The in-between writes lerp(N-1, N, alpha) into [1][view], replays, and restores N.
//
// Live control, no rebuild cycle (the probe exists for exactly this):
//   curl '127.0.0.1:17654/interp60?alpha=0.5'      blend factor, 0 = previous tick, 1 = this tick
//   curl '127.0.0.1:17654/interp60?on=0'           A/B the whole feature against itself
//   curl  127.0.0.1:17654/interp60                 counters: models blended, bails, motion
//
//   SBR_INTERP60=1   enable (off by default while the effect passes are still being worked out)

#include "overrides.h"

#include "../runtime/probe_server.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <vector>

extern "C" void func_802deeb8(CPUState&);   // J3DModel::viewCalc

namespace {

constexpr u32 J3DMODEL_VIEWCALC = 0x802deeb8u;

// J3DModel layout, from the retired RE:
//   +0x04 mModelData   +0x60 mDrawMtxBuf[0]   +0x64 mDrawMtxBuf[1]   +0x7C view index
// J3DModelData:
//   +0x98 mDrawMtxData.mEntryNum (u16)
// A draw matrix is a 3x4 of f32 = 48 bytes.
constexpr u32 MODEL_DATA = 0x04, MODEL_BUF0 = 0x60, MODEL_BUF1 = 0x64, MODEL_VIEW = 0x7C;
constexpr u32 MODELDATA_ENTRYNUM = 0x98;
constexpr u32 MTX_BYTES = 48, MTX_FLOATS = 12;

struct Config {
    bool  enabled = false;
    float alpha = 0.5f;
};
Config g_cfg;

struct Stats {
    unsigned long fields = 0, models = 0, blended = 0;
    unsigned long bail_bad = 0, bail_single = 0, bail_new = 0;
    double move_max = 0;
};
Stats g_stats;

// Every J3DModel the real field's viewCalc touched. viewCalc runs once per model per frame, so
// this is exactly the set of live models, and each one's double buffer holds both ticks.
std::vector<u32> g_registry;
std::unordered_set<u32> g_seen;

// The models that were live on the PREVIOUS field. A model that first appears this field has no
// previous tick: its mDrawMtxBuf[0] holds whatever the allocator last left there, and interpolating
// from that is not "half-way", it is half-way to garbage. Measured before this gate existed: a
// joint delta of 1.2e27, which blends to a vertex flung across the screen.
std::unordered_set<u32> g_liveLastField;

// Tick-N matrices, saved before the blend overwrites them so the guest's own double buffer is left
// exactly as the real field made it.
struct Saved {
    u32 addr = 0;
    std::vector<float> f;
};
std::vector<Saved> g_restore;

bool ok_ram(u32 a) { return sb_ram_fast(a) != nullptr; }

f32 guest_f32(u32 ea) {
    const u32 bits = sb_r32(ea);
    f32 f;
    __builtin_memcpy(&f, &bits, sizeof f);
    return f;
}

void guest_set_f32(u32 ea, f32 v) {
    u32 bits;
    __builtin_memcpy(&bits, &v, sizeof bits);
    sb_w32(ea, bits);
}

// Blend one model's draw matrices toward the previous tick. Returns false, without touching
// anything, for every shape of model that cannot be interpolated — a model that appeared this
// frame has no previous tick, and guessing one would teleport it from wherever the buffer
// happened to point.
bool blend_model(u32 model, float alpha) {
    if (!ok_ram(model)) { ++g_stats.bail_bad; return false; }
    if (g_liveLastField.count(model) == 0) { ++g_stats.bail_new; return false; }   // no previous tick
    const u32 data = sb_r32(model + MODEL_DATA);
    const u32 view = sb_r32(model + MODEL_VIEW);
    if (!ok_ram(data) || view > 16) { ++g_stats.bail_bad; return false; }

    const u32 n = sb_r16(data + MODELDATA_ENTRYNUM);
    const u32 b0 = sb_r32(model + MODEL_BUF0), b1 = sb_r32(model + MODEL_BUF1);
    if (n == 0 || n > 512 || !ok_ram(b0) || !ok_ram(b1)) { ++g_stats.bail_bad; return false; }

    const u32 prev = sb_r32(b0 + 4 * view);   // tick N-1
    const u32 cur  = sb_r32(b1 + 4 * view);   // tick N — what the shape packets load
    // The matrix arrays are allocated 0x20-aligned. Anything else is not one of them, and a write
    // through a mis-derived pointer corrupts unrelated state and faults somewhere else entirely.
    if (!ok_ram(prev) || !ok_ram(cur) || (prev & 0x1F) || (cur & 0x1F)) { ++g_stats.bail_bad; return false; }
    if (!ok_ram(cur + n * MTX_BYTES - 1)) { ++g_stats.bail_bad; return false; }
    if (prev == cur) { ++g_stats.bail_single; return false; }   // single-buffered: no previous tick

    g_restore.push_back({});
    Saved& s = g_restore.back();
    s.addr = cur;
    s.f.resize((size_t)n * MTX_FLOATS);
    for (u32 i = 0; i < n * MTX_FLOATS; i++) s.f[i] = guest_f32(cur + i * 4);

    double moved = 0;
    for (u32 i = 0; i < n; i++) {
        const u32 pm = prev + i * MTX_BYTES, cm = cur + i * MTX_BYTES;
        // Translation is column 3 of each row: elements 3, 7, 11.
        float dd = 0;
        for (u32 t = 3; t < MTX_FLOATS; t += 4) {
            const float d = guest_f32(cm + t * 4) - guest_f32(pm + t * 4);
            dd += d * d;
        }
        // A joint whose delta is not FINITE has a garbage previous tick — an uninitialised buffer,
        // or a model whose matrix array was reused. NaN alone is not enough of a test: infinities
        // reached the blend and produced non-finite matrices, which is a wild transform on the
        // in-between field. Keep tick N for that joint rather than interpolating from nonsense.
        if (!std::isfinite(dd)) continue;
        moved += dd;
        for (u32 f = 0; f < MTX_FLOATS; f++)
            guest_set_f32(cm + f * 4, (1.0f - alpha) * guest_f32(pm + f * 4) + alpha * guest_f32(cm + f * 4));
    }
    moved /= n;
    if (moved == moved && moved > g_stats.move_max) g_stats.move_max = moved;

    ++g_stats.blended;
    return true;
}

void ov_view_calc(CPUState& cpu) {
    const u32 model = cpu.gpr[3];
    func_802deeb8(cpu);
    if (!g_cfg.enabled) return;
    // Record after the guest body: the double buffer has just been swapped and recomputed, so
    // [0] and [1] now hold the two ticks this frame can interpolate between.
    if (ok_ram(model) && g_seen.insert(model).second) g_registry.push_back(model);
}

const bool g_probe = [] {
    sb_probe_register("/interp60", "60fps interpolation: alpha=<0..1> on=<0|1>; reports counters",
                      [](const ProbeArgs& a) {
                          if (a.has("alpha")) g_cfg.alpha = (float)a.num("alpha", g_cfg.alpha);
                          if (a.has("on")) g_cfg.enabled = a.integer("on", 1) != 0;
                          char buf[320];
                          std::snprintf(buf, sizeof buf,
                                        "enabled=%d alpha=%.3f\n"
                                        "fields=%lu models=%lu blended=%lu\n"
                                        "bail_bad=%lu bail_single=%lu bail_new=%lu\n"
                                        "max_joint_motion=%.3f\n",
                                        (int)g_cfg.enabled, (double)g_cfg.alpha, g_stats.fields,
                                        g_stats.models, g_stats.blended, g_stats.bail_bad,
                                        g_stats.bail_single, g_stats.bail_new, g_stats.move_max);
                          return std::string(buf);
                      });
    return true;
}();

} // namespace

// Enabled? Read once, but the probe can flip it live afterwards.
bool sbr_interp60() {
    static bool init = false;
    if (!init) {
        init = true;
        const char* e = std::getenv("SBR_INTERP60");
        g_cfg.enabled = (e != nullptr && e[0] != '\0' && e[0] != '0');
        if (const char* a = std::getenv("SBR_INTERP60_ALPHA")) g_cfg.alpha = (float)std::atof(a);
        if (g_cfg.enabled)
            lucent::info("interp60", "60fps interpolation on (alpha {:.2f}) — /interp60 to tune live",
                         g_cfg.alpha);
    }
    return g_cfg.enabled;
}

// Blend every live model toward the previous tick. Returns false if there is nothing to show, in
// which case the caller must NOT present an in-between field.
float sbr_interp60_alpha() { return g_cfg.alpha; }

// This field's live model set becomes the next field's "was already here" test, then clears for the
// next frame's viewCalc records. Runs every frame (see the note in sbr_interp60_blend).
static void rotate_live_set() {
    g_liveLastField.clear();
    g_liveLastField.insert(g_registry.begin(), g_registry.end());
    g_registry.clear();
    g_seen.clear();
}

bool sbr_interp60_blend() {
    if (!g_cfg.enabled) { rotate_live_set(); return false; }
    ++g_stats.fields;
    g_stats.models = g_registry.size();
    g_stats.blended = 0;
    g_restore.clear();
    for (u32 model : g_registry) blend_model(model, g_cfg.alpha);
    // Rotate the live set NOW — every frame, even one that blended nothing. The gate that rejects
    // freshly-spawned models reads it, so if it only advanced on frames that produced an in-between,
    // the very first frame (nothing live "last field" yet) would bail forever and the in-between
    // would never start. Restore of the guest RAM is separate (sbr_interp60_restore), and only
    // needed when something was blended.
    const bool blended = !g_restore.empty();
    rotate_live_set();
    return blended;
}

// Put tick N back, exactly as the real field computed it. This MUST run before the game's next
// field, or the blended values become the base for the following interpolation and the scene
// drifts backwards a fraction of a tick every frame.
void sbr_interp60_restore() {
    for (const Saved& s : g_restore)
        for (size_t i = 0; i < s.f.size(); i++) guest_set_f32(s.addr + (u32)i * 4, s.f[i]);
    g_restore.clear();
}

SB_OVERRIDE(J3DMODEL_VIEWCALC, ov_view_calc, "J3DModel::viewCalc",
            "60fps interpolation: record the live models, whose draw-matrix double buffer holds "
            "both ticks")
