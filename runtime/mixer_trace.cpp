// Host audio flow meter (linker --wrap, no Dolphin modification).
//
// Diagnostic for "the DSP mix WAV is continuous but the speakers play only the first instant"
// (2026-06-11): the dump is written per pushed buffer WITHOUT real-time pacing, so it cannot
// distinguish a healthy stream from one that starves in real time. This wrap counts, per
// wall-clock second on stderr: samples PUSHED into Dolphin's mixer (the emulated AI DMA side)
// vs samples PULLED by the audio backend callback (Cubeb side). Healthy ≈ 32k/48k in and out;
// pushes ≪ pulls = the emulated side starves the host (CoreTiming pacing); pushes ≈ rate but
// pulls ≈ 0 = backend dead. Permanent, env-gated: SUNBRIGHT_DBG_MIXER=1.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
std::atomic<uint64_t> g_pushed{0}, g_pulled{0}, g_nonzero{0}, g_out_nonzero{0};
bool dbg() { static const bool on = getenv("SUNBRIGHT_DBG_MIXER") != nullptr; return on; }
void tick(const char* who) {
    if (!dbg()) return;
    static std::atomic<int64_t> last{0};
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t prev = last.load(std::memory_order_relaxed);
    if (now != prev && last.compare_exchange_strong(prev, now)) {
        static uint64_t lp = 0, lq = 0;
        const uint64_t p = g_pushed.load(), q = g_pulled.load();
        static uint64_t lz = 0;
        const uint64_t z = g_nonzero.load();
        static uint64_t lo = 0;
        const uint64_t o = g_out_nonzero.load();
        fprintf(stderr, "[mixer] %s push=%llu/s (nonzero=%llu/s) pull=%llu/s (out_nonzero=%llu/s)\n",
                who, (unsigned long long)(p - lp), (unsigned long long)(z - lz),
                (unsigned long long)(q - lq), (unsigned long long)(o - lo));
        lp = p; lq = q; lz = z; lo = o;
    }
}
}  // namespace

extern "C" void __real__ZN5Mixer11PushSamplesEPKsm(void* self, const short* samples, size_t n);
extern "C" void __wrap__ZN5Mixer11PushSamplesEPKsm(void* self, const short* samples, size_t n) {
    g_pushed.fetch_add(n, std::memory_order_relaxed);
    uint64_t nz = 0;
    for (size_t i = 0; i < n * 2; i++) nz += samples[i] != 0;
    g_nonzero.fetch_add(nz / 2, std::memory_order_relaxed);
    tick("push");
    __real__ZN5Mixer11PushSamplesEPKsm(self, samples, n);
}

extern "C" size_t __real__ZN5Mixer3MixEPsm(void* self, short* samples, size_t n);
extern "C" size_t __wrap__ZN5Mixer3MixEPsm(void* self, short* samples, size_t n) {
    g_pulled.fetch_add(n, std::memory_order_relaxed);
    const size_t r = __real__ZN5Mixer3MixEPsm(self, samples, n);
    uint64_t nz = 0;
    for (size_t i = 0; i < n * 2; i++) nz += samples[i] != 0;
    g_out_nonzero.fetch_add(nz / 2, std::memory_order_relaxed);
    tick("pull");
    return r;
}
