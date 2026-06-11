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

// DSP-fifo fill estimate in ms of output time: samples pushed (resampled 32028 -> 48000)
// minus samples pulled by the backend. The governor's audio servo reads this to keep a
// cushion in front of the sink — Dolphin's granule mixer LOOPS its last granule when the
// queue runs dry (m_queue_looping), which is the audible skip/warble. Negative = the sink
// has consumed more than was ever produced (chronically dry). Approximate (fixed 32028
// ratio, ignores DTK) — fine for a servo signal, not an absolute fill.
// Returns LONG_MIN while the backend is not pulling yet (no audio clock to follow).
extern "C" long sunbright_audio_fill_ms() {
    const uint64_t pushed = g_pushed.load(std::memory_order_relaxed);
    const uint64_t pulled = g_pulled.load(std::memory_order_relaxed);
    if (pulled == 0 || pushed == 0) return LONG_MIN;   // no audio clock yet
    const long long fill = (long long)(pushed * 48000.0 / 32028.0) - (long long)pulled;
    return (long)(fill * 1000 / 48000);
}

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
    // Prime the sink ONCE with ~50 ms of silence before the first real sample. The granule
    // queue starts at zero depth, and the host-clock governor holds production at exactly the
    // consumption rate — a cushion can never build on its own, so the queue runs borderline-dry
    // forever and every scheduling hiccup longer than a granule trips Dolphin's dry-queue
    // behavior: jump the playhead back half a queue and LOOP it (the audible skip/warble of the
    // boot jingle). A one-time 50 ms latency is imperceptible; the 1:1 lock then maintains it.
    {
        static const bool primed = [self] {
            static short silence[1600 * 2] = {};   // 1600 frames @32028 ≈ 50 ms
            g_pushed.fetch_add(1600, std::memory_order_relaxed);
            __real__ZN5Mixer11PushSamplesEPKsm(self, silence, 1600);
            return true;
        }();
        (void)primed;
    }
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
