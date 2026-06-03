#pragma once
// Built-in HTTP/JSON probe server for live, headless performance measurement.
//
// Enabled by SUNBRIGHT_PROBE=1 (port via SUNBRIGHT_PROBE_PORT, default 17654). Serves
// GET /metrics → a JSON snapshot of emulation speed (Dolphin perf metrics) plus our own
// dispatch counters (recomp vs interpreter call mix, interpreter step rate, poll-yields).
// The point: the recomp runs on its own threads with no console FPS readout, so to answer
// "why is it slow / is it below real-time and where do the cycles go" you probe the live
// process instead of guessing. Run the game in the background, `curl localhost:17654/metrics`.
//
// Counters are only touched when g_probe_enabled (predictable branch; zero cost otherwise),
// so leaving the instrumentation compiled in is free for normal runs.
#include <cstdint>
#include <atomic>

struct ProbeCounters {
    std::atomic<uint64_t> call_recomp{0};     // bl/bctrl into a recompiled function (the fast path)
    std::atomic<uint64_t> call_interp{0};     // bl/bctrl into a non-recomp function → Dolphin interpreter
    std::atomic<uint64_t> call_native_os{0};  // bl/bctrl into a native-OS override
    std::atomic<uint64_t> tail{0};            // tail_ppc (b/bctr leaving a function)
    std::atomic<uint64_t> interp_steps{0};    // interpreter SingleStep count (the expensive instruction path)
    std::atomic<uint64_t> poll_yield{0};      // sunbright_poll_yield invocations (idle/IRQ pumps)
    std::atomic<uint64_t> interp_ns{0};       // wall-clock ns spent inside the Dolphin interpreter loop
};

extern ProbeCounters g_probe;
extern bool g_probe_enabled;

// Starts the HTTP thread if SUNBRIGHT_PROBE is set. Idempotent; safe to call once at boot.
void probe_server_start();
