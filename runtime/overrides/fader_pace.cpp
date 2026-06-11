// Boot pacing signal #2: the first timed visual (TSMSFader::startWipe) — not just first audio.
//
// Boot runs uncapped until the first audible sample (PC-game rule: load as fast as the machine
// allows). But the GC-logo fade-IN happens BEFORE any audio: TGCLogoDir::setup calls
// startWipe(14, 0.4s) and the boot jingle only starts once isFullyFadedIn() — so the whole
// fade ran at uncapped frame rate and completed in milliseconds (logo popped in fully visible,
// 2026-06-12). A wipe is user-visible timed content by definition: the moment the game requests
// one, frame timing is meaningful and pacing must engage.
#include <atomic>
#include <cstdio>
#include <cstdint>

#include "../cpu_state.h"
#include "../overrides.h"

extern "C" void func_8013f860(CPUState& cpu);   // TSMSFader::startWipe(u32, f32, f32)

static std::atomic<bool> g_visual_live{false};

extern "C" bool sb_visual_live() { return g_visual_live.load(std::memory_order_relaxed); }

SUNBRIGHT_OVERRIDE(ov_fader_startWipe, 0x8013f860u) {
    if (!g_visual_live.exchange(true, std::memory_order_relaxed))
        fprintf(stderr, "[fader] first startWipe — frame pacing engages\n");
    func_8013f860(cpu);
}
