// Loss-free PE draw-sync token capture (root cause of the PE-token-era crawl, 2026-06-10).
//
// Dolphin's PixelEngineManager::SetToken coalesces: `m_token_pending = token` overwrites any
// token whose SetTokenFinish event hasn't run yet, and only the LATEST value survives. SMS ends
// every frame with TWO back-to-back tokens (the pollution-range token, e.g. 0x7d, then the
// frame-done token 0 — TDrawSyncManager protocol, reference/sms/src/System/DrawSyncManager.cpp),
// so one of the two was dropped nearly every frame. The TDrawSyncManager fifo then missed an
// advance, the GPU parked at the CP breakpoint, and the pipeline crawled forward only via the
// idle-driver's synthetic-token recovery (~3 s/frame, the vi.gpu_backpressure busy-spin).
//
// On real hardware every token interrupt is delivered; the game's counting protocol is sound.
// So we own token delivery: this linker --wrap records EVERY interrupt-worthy token value, in
// order, on the video thread; the native PE_TOKEN dispatch (dolphin_hook.cpp) drains the ring
// and runs the GX token callback once per value — no coalescing, no loss. __real_SetToken still
// runs so Dolphin's m_token register/interrupt bookkeeping stays consistent.
#include <atomic>
#include <cstdint>

namespace {
constexpr unsigned kCap = 1024;                       // tokens/frame is ~2; this never fills
std::atomic<uint32_t> g_w{0}, g_r{0};
uint16_t g_ring[kCap];
}

// Consumer (CPU thread, native PE_TOKEN dispatch). Returns false when empty.
bool sb_token_ring_pop(uint16_t* out) {
    const uint32_t r = g_r.load(std::memory_order_relaxed);
    if (r == g_w.load(std::memory_order_acquire)) return false;
    *out = g_ring[r % kCap];
    g_r.store(r + 1, std::memory_order_release);
    return true;
}

// Original Dolphin body, exposed by the fork (replaces the --wrap __real_).
extern "C" void sb_pe_set_token_impl(void* self, uint16_t token, bool interrupt,
                                     int cycles_into_future);

// Fork hook (installed via sb_install_hooks → sb_hook_pe_set_token). Same body as the old
// __wrap_; calls sb_pe_set_token_impl instead of __real_ to run Dolphin's original path.
extern "C" void sb_hook_pe_set_token(void* self, uint16_t token, bool interrupt,
                                     int cycles_into_future) {
    if (interrupt) {
        const uint32_t w = g_w.load(std::memory_order_relaxed);
        if (w - g_r.load(std::memory_order_acquire) < kCap) {   // full ring: drop (never happens
            g_ring[w % kCap] = token;                           // at 2 tokens/frame; consumer
            g_w.store(w + 1, std::memory_order_release);        // drains every dispatch)
        }
    }
    sb_pe_set_token_impl(self, token, interrupt, cycles_into_future);
}
