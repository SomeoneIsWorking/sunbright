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
#include <climits>

namespace {
std::atomic<uint64_t> g_pushed{0}, g_pulled{0}, g_nonzero{0}, g_out_nonzero{0};
}  // namespace

// The governor's audio-clock signal (sunbright_audio_fill_ms) lives in native_audio.cpp now —
// it reads the NATIVE sink's DSP ring fill directly instead of a push/pull estimate.
extern "C" void na_push_dsp(const int16_t* s, size_t n);
extern "C" void na_push_dtk(const int16_t* s, size_t n);
extern "C" void na_set_dsp_rate(uint32_t rate);
extern "C" void na_set_dtk_rate(uint32_t rate);
extern "C" void na_set_dtk_volume(int l, int r);

namespace {
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

// SUNBRIGHT_DBG_MIXER_BURST=N: log every push/pull with a wall-clock ms timestamp for the
// first N seconds — burstiness is invisible in the per-second sums (a correct 32k/s average
// delivered as 100 ms lumps still overflows/underruns the backend ring and skips audibly).
namespace {
double burst_window() {
    static const double w = [] {
        const char* e = getenv("SUNBRIGHT_DBG_MIXER_BURST");
        return e ? atof(e) : 0.0;
    }();
    return w;
}
double now_s() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

extern "C" void __real__ZN5Mixer11PushSamplesEPKsm(void* self, const short* samples, size_t n);
extern "C" void __wrap__ZN5Mixer11PushSamplesEPKsm(void* self, const short* samples, size_t n) {
    na_push_dsp(samples, n);                 // native sink: the only real consumer
    g_pushed.fetch_add(n, std::memory_order_relaxed);
    uint64_t nz = 0;
    for (size_t i = 0; i < n * 2; i++) nz += samples[i] != 0;
    g_nonzero.fetch_add(nz / 2, std::memory_order_relaxed);
    const double t = now_s();
    if (t < burst_window())
        fprintf(stderr, "[mixb] %9.4f push %5zu nz=%llu\n", t, n, (unsigned long long)nz / 2);
    tick("push");
    __real__ZN5Mixer11PushSamplesEPKsm(self, samples, n);
}

// SUNBRIGHT_DBG_MIXER_OUT=path: record what the backend PULLS (the audible stream) as raw
// s16 stereo 48 kHz, in WALL time — the dspdump WAV records pushes in content time and by
// construction cannot show sink-side jitter. Underruns/repeats appear here as the ear hears
// them. Pair with [mixfill]: per second, the min/max backlog estimate (pushed resampled to
// output rate minus pulled); min ~0 = the sink ran dry that second (audible stutter).
extern "C" size_t __real__ZN5Mixer3MixEPsm(void* self, short* samples, size_t n);
extern "C" size_t __wrap__ZN5Mixer3MixEPsm(void* self, short* samples, size_t n) {
    g_pulled.fetch_add(n, std::memory_order_relaxed);
    const double t = now_s();
    if (t < burst_window())
        fprintf(stderr, "[mixb] %9.4f pull %5zu\n", t, n);
    {
        static FILE* out = [] {
            const char* p = getenv("SUNBRIGHT_DBG_MIXER_OUT");
            return p ? fopen(p, "wb") : nullptr;
        }();
        static std::atomic<int64_t> fill_min{INT64_MAX}, fill_max{INT64_MIN};
        static std::atomic<int64_t> last_sec{0};
        if (dbg() || out) {
            // backlog estimate in output-rate samples (DSP 32028 → 48000: ×1.4987)
            const int64_t fill = (int64_t)(g_pushed.load() * 48000.0 / 32028.0) -
                                 (int64_t)(g_pulled.load() + n);
            int64_t mn = fill_min.load();
            while (fill < mn && !fill_min.compare_exchange_weak(mn, fill)) {}
            int64_t mx = fill_max.load();
            while (fill > mx && !fill_max.compare_exchange_weak(mx, fill)) {}
            const int64_t sec = (int64_t)t;
            int64_t prev = last_sec.load();
            if (sec != prev && last_sec.compare_exchange_strong(prev, sec)) {
                fprintf(stderr, "[mixfill] t=%llds fill_min=%lldms fill_max=%lldms\n",
                        (long long)sec, (long long)(fill_min.exchange(INT64_MAX) * 1000 / 48000),
                        (long long)(fill_max.exchange(INT64_MIN) * 1000 / 48000));
            }
        }
        const size_t r = __real__ZN5Mixer3MixEPsm(self, samples, n);
        if (out) fwrite(samples, 4, n, out);
        uint64_t nz2 = 0;
        for (size_t i = 0; i < n * 2; i++) nz2 += samples[i] != 0;
        g_out_nonzero.fetch_add(nz2 / 2, std::memory_order_relaxed);
        tick("pull");
        return r;
    }
}

// ── native-sink feed wraps (the port: our SDL sink consumes; Dolphin's backend is null) ──────
extern "C" void __real__ZN5Mixer20PushStreamingSamplesEPKsm(void* self, const short* s, size_t n);
extern "C" void __wrap__ZN5Mixer20PushStreamingSamplesEPKsm(void* self, const short* s, size_t n) {
    na_push_dtk(s, n);
    __real__ZN5Mixer20PushStreamingSamplesEPKsm(self, s, n);   // keep Dolphin's DTK dump working
}

// Exact input rates: rate = FIXED_SAMPLE_RATE_DIVIDEND (108000000) / divisor.
extern "C" void __real__ZN5Mixer28SetDMAInputSampleRateDivisorEj(void* self, unsigned div);
extern "C" void __wrap__ZN5Mixer28SetDMAInputSampleRateDivisorEj(void* self, unsigned div) {
    if (div) na_set_dsp_rate(108000000u / div);
    __real__ZN5Mixer28SetDMAInputSampleRateDivisorEj(self, div);
}
extern "C" void __real__ZN5Mixer31SetStreamInputSampleRateDivisorEj(void* self, unsigned div);
extern "C" void __wrap__ZN5Mixer31SetStreamInputSampleRateDivisorEj(void* self, unsigned div) {
    if (div) na_set_dtk_rate(108000000u / div);
    __real__ZN5Mixer31SetStreamInputSampleRateDivisorEj(self, div);
}
extern "C" void __real__ZN5Mixer18SetStreamingVolumeEjj(void* self, unsigned l, unsigned r);
extern "C" void __wrap__ZN5Mixer18SetStreamingVolumeEjj(void* self, unsigned l, unsigned r) {
    na_set_dtk_volume((int)l, (int)r);
    __real__ZN5Mixer18SetStreamingVolumeEjj(self, l, r);
}
