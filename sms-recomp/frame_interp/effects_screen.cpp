// screen_effects.cpp — own the identification of the game's screen-space effects.
//
// See docs/60fps/screen_effects.md for the catalog. This hooks each effect's guest `perform`, runs
// the real body (the recomp owns the effect by executing its real code), and records that it fired
// — so interp60 and any future consumer can ask "which screen-sampling effects drew this frame" by
// name instead of pattern-matching an opaque GX stream. That named signal is the whole point: it
// turns a black-box interaction into an owned one.
//
// TShimmer and the water refraction are hooked HERE. TAfterEffect and TBathWaterManager::draw_mist
// and TMirrorCamera are already hooked by widescreen_effects.cpp (a different concern, one override
// per address); those call sb_screen_effect_fired() directly rather than being double-hooked.

#include "../overrides/overrides.h"

#include "../runtime/probe_server.h"
#include "app/settings.h"
#include "effects.h"

#include <intrinsics.h>

#include <atomic>
#include <cstdio>

extern "C" void func_8019f83c(CPUState&); // TShimmer::perform
extern "C" void func_8027c12c(CPUState&); // TModelWaterManager::drawRefracAndSpec

namespace {

// Two masks: what fired THIS frame (published to consumers) and what is accumulating for the frame
// in progress. Rolled over by sb_screen_effects_frame_end() so a query mid-frame sees a stable
// last-frame value.
std::atomic<u32> g_published{0};
std::atomic<u32> g_accum{0};

// Per-effect lifetime fire/draw tallies, for the /screenfx probe — "did this effect ever run" is a
// question the census answers without a rebuild.
struct Tally {
    std::atomic<unsigned long> fired{0}, drew{0};
};
Tally g_tally[5]; // indexed by bit position of ScreenEffect

int bit_index(u32 mask) {
    for (int i = 0; i < 5; ++i)
        if (mask == (1u << i))
            return i;
    return 0;
}

const bool g_probe = [] {
    sb_probe_register("/screenfx", "screen-space effects: fired/drew this session, and last frame",
                      [](const ProbeArgs&) {
                          static const char* kName[5] = {"shimmer(haze)", "dash-blur",
                                                         "water-refrac", "bath-mist",
                                                         "mirror-prerender"};
                          std::string out;
                          char buf[128];
                          for (int i = 0; i < 5; ++i) {
                              std::snprintf(buf, sizeof buf, "%-18s fired=%lu drew=%lu\n", kName[i],
                                            g_tally[i].fired.load(), g_tally[i].drew.load());
                              out += buf;
                          }
                          std::snprintf(buf, sizeof buf, "last-frame capture-sampling active: %d\n",
                                        (int)sb_screen_sampling_active());
                          out += buf;
                          return out;
                      });
    return true;
}();

// TShimmer::perform(u32 flags, TGraphics*): the draw branch is `flags & 4` (the branch that builds
// the effect matrix and enters the mesh). It early-outs entirely while FLUDD is emitting, but that
// is decided inside the body; `flags & 4` is the closest pre-call signal for "this is a draw pass".
// The "Heat Haze" setting (app::settings, toggled from the in-game menu) disables the effect by
// skipping the guest body entirely — the mesh is never drawn and the screen capture is not sampled.
void ov_shimmer(CPUState& cpu) {
    if (!sb::app::settings().effective().hazeEnabled)
        return;
    const bool drew = (cpu.gpr[4] & 4) != 0;
    func_8019f83c(cpu);
    sb_screen_effect_fired(ScreenEffect::Shimmer, drew);
}

// TModelWaterManager::drawRefracAndSpec(): a const method with no flags — reaching it IS the draw.
void ov_water_refrac(CPUState& cpu) {
    func_8027c12c(cpu);
    sb_screen_effect_fired(ScreenEffect::WaterRefraction, true);
}

} // namespace

// ── Registry API (screen_effects.h) ──────────────────────────────────────────────────────
void sb_screen_effect_fired(ScreenEffect e, bool drew) {
    const u32 bit = (u32)e;
    g_tally[bit_index(bit)].fired.fetch_add(1, std::memory_order_relaxed);
    if (drew) {
        g_tally[bit_index(bit)].drew.fetch_add(1, std::memory_order_relaxed);
        g_accum.fetch_or(bit, std::memory_order_relaxed);
    }
}

u32 sb_screen_effects_this_frame() {
    return g_published.load(std::memory_order_relaxed);
}

bool sb_screen_sampling_active() {
    return (sb_screen_effects_this_frame() & kScreenSamplingEffects) != 0;
}

void sb_screen_effects_frame_end() {
    g_published.store(g_accum.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
}

SB_OVERRIDE(0x8019f83cu, ov_shimmer, "TShimmer::perform",
            "own the heat-haze identity: run the real effect, record that it sampled the screen")
SB_OVERRIDE(0x8027c12cu, ov_water_refrac, "TModelWaterManager::drawRefracAndSpec",
            "own the water-refraction identity: run the real effect, record the screen sample")
