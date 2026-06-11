// PC-native audio sink — owns audio DELIVERY end to end (user directive: port, don't tweak).
//
// What was wrong (observed, 2026-06-11): the GameCube DSP is an independent processor — on
// hardware the speaker is clocked by the AI FIFO and audio cannot skip, no matter what the CPU
// does. Under the hybrid, DSP samples were delivered through Dolphin's granule Mixer to a Cubeb
// stream, and the granule mixer reacts to a dry queue by jumping the playhead BACK and
// REPLAYING the last half-queue (Mixer.cpp Dequeue) — every production hiccup longer than a
// granule becomes an audible loop/warble (the skipping Nintendo-logo jingle). Pacing knob fixes
// (governor, cushion prime) cannot remove the mechanism.
//
// The port: this file replaces the whole delivery path.
//   game (recomp) → DSP HLE mix → Mixer::PushSamples ─┐ (wrap, mixer_trace.cpp calls in here)
//                                                     ├→ OUR ring buffers (DSP 32028, DTK 48043)
//   Dolphin backend = "No Audio Output" (nothing else consumes)
//   SDL audio device @48 kHz — the callback IS the audio clock:
//     · per-ring linear resampler with a fill servo (±0.5% rate trim toward a 60 ms target —
//       inaudible, absorbs clock drift between the emulated 32028 Hz and the device clock)
//     · a starting gate: output silence until the ring first reaches the target fill, so
//       playback never begins on an empty buffer (re-arms after a sustained underrun, e.g.
//       a load stall — playback resumes only once the cushion is back)
//     · underrun → SILENCE (counted, logged), never replayed audio. A silent gap is the
//       faithful behavior of a starved DAC; a looped granule is not.
//
// The DSP-ring fill is also exported as the governor's audio-clock signal
// (sunbright_audio_fill_ms — emulated time leads the host clock while the sink needs refill).
// SUNBRIGHT_MUTE / headless zero the output buffer but keep identical timing, so audio bugs
// reproduce headless. SUNBRIGHT_DBG_NAUDIO=1 logs per-second sink stats.
#include <SDL.h>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int kOutRate     = 48000;
// THRESHOLD ORDER MATTERS (boot deadlock, 2026-06-11): production is governed by
// sunbright_audio_fill_ms() < kTargetMs (the governor stops advancing emulated time once the
// ring holds kTargetMs), and the starting gate begins consumption at kGateMs. The gate MUST sit
// BELOW the governor's stop level or neither side moves: fill parks between them, the gate
// never opens, the sink never drains, and the governor holds emulated time frozen forever
// (boot froze at 4 VI fields with fill pinned at 53 ms when the gate was 60 and the stop 50).
constexpr int kTargetMs    = 80;     // servo fill target == the governor's production stop level
constexpr int kGateMs      = 60;     // starting gate: begin/resume playback at this fill
constexpr int kMaxTrim     = 0.005 * (1 << 16);  // ±0.5% rate trim, 16.16 fixed point
constexpr size_t kRingCap  = 1 << 16;            // frames (~2 s @32 kHz) — power of two

struct Ring {
    int16_t   data[kRingCap * 2];      // interleaved stereo
    std::atomic<uint64_t> w{0};        // frames written (producer: emu thread)
    std::atomic<uint64_t> r{0};        // frames consumed (consumer: SDL audio thread)
    std::atomic<uint32_t> in_rate{0};  // input sample rate (0 = stream never started)
    uint64_t  pos_frac = 0;            // consumer resample position, 32.32 fixed point
    bool      gate_open = false;       // starting gate (see header comment)
    std::atomic<uint64_t> underruns{0};

    void push(const int16_t* s, size_t n) {
        uint64_t wi = w.load(std::memory_order_relaxed);
        const uint64_t ri = r.load(std::memory_order_acquire);
        if (wi + n - ri > kRingCap) {         // full (consumer stalled): drop oldest by advancing r
            r.store(wi + n - kRingCap, std::memory_order_release);
        }
        for (size_t i = 0; i < n; i++) {
            const size_t slot = (wi + i) & (kRingCap - 1);
            data[slot * 2 + 0] = s[i * 2 + 0];
            data[slot * 2 + 1] = s[i * 2 + 1];
        }
        w.store(wi + n, std::memory_order_release);
    }

    uint64_t avail() const {
        return w.load(std::memory_order_acquire) - r.load(std::memory_order_relaxed);
    }

    long fill_ms() const {
        const uint32_t rate = in_rate.load(std::memory_order_relaxed);
        return rate ? (long)(avail() * 1000 / rate) : LONG_MIN;
    }

    // Resample-mix `frames` output frames into out (additive). Returns frames actually mixed
    // (the rest were left untouched = silence from the caller's pre-cleared buffer).
    size_t mix_into(int32_t* out, size_t frames) {
        const uint32_t rate = in_rate.load(std::memory_order_relaxed);
        if (!rate) return 0;
        const long target = (long)rate * kTargetMs / 1000;
        const long have   = (long)avail();
        if (!gate_open) {
            if (have < (long)rate * kGateMs / 1000) return 0;   // gate: silent until cushioned
            gate_open = true;
        }
        // Fill servo: trim the resample step toward holding `target` frames buffered.
        int64_t step = ((int64_t)rate << 16) / kOutRate;             // 16.16 input-frames/output-frame
        const int64_t trim = (have - target) * kMaxTrim / target;    // proportional, saturating
        step += (trim > kMaxTrim) ? kMaxTrim : (trim < -kMaxTrim) ? -kMaxTrim : trim;
        uint64_t ri = r.load(std::memory_order_relaxed);
        const uint64_t wi = w.load(std::memory_order_acquire);
        size_t produced = 0;
        uint64_t pos = ((uint64_t)ri << 32) | pos_frac;              // absolute 32.32 input position
        for (; produced < frames; produced++) {
            const uint64_t idx = pos >> 32;
            if (idx + 1 >= wi) break;                                 // need idx and idx+1
            const uint32_t frac = (uint32_t)(pos >> 16) & 0xFFFF;     // 16-bit interp fraction
            const size_t a = idx & (kRingCap - 1), b = (idx + 1) & (kRingCap - 1);
            const int32_t l = data[a * 2] + (((data[b * 2] - data[a * 2]) * (int32_t)frac) >> 16);
            const int32_t rr = data[a * 2 + 1] + (((data[b * 2 + 1] - data[a * 2 + 1]) * (int32_t)frac) >> 16);
            out[produced * 2 + 0] += l;
            out[produced * 2 + 1] += rr;
            pos += (uint64_t)step << 16;                              // 16.16 → 32.32
        }
        pos_frac = pos & 0xFFFFFFFFull;
        r.store(pos >> 32, std::memory_order_release);
        if (produced < frames) {
            underruns.fetch_add(1, std::memory_order_relaxed);
            gate_open = false;                                        // re-arm: resume only cushioned
        }
        return produced;
    }
};

Ring g_dsp, g_dtk;
std::atomic<int> g_dtk_vol_l{256}, g_dtk_vol_r{256};
std::atomic<bool> g_muted{false};
std::atomic<uint64_t> g_cb_count{0};
SDL_AudioDeviceID g_dev = 0;

bool dbg() { static const bool on = getenv("SUNBRIGHT_DBG_NAUDIO") != nullptr; return on; }

void sdl_callback(void*, Uint8* stream, int len) {
    const size_t frames = (size_t)len / 4;            // s16 stereo
    static int32_t acc[4096 * 2];
    if (frames * 2 > sizeof(acc) / sizeof(acc[0])) { memset(stream, 0, len); return; }
    memset(acc, 0, frames * 2 * sizeof(int32_t));
    g_dsp.mix_into(acc, frames);
    {   // DTK with AI volume
        static int32_t dtk[4096 * 2];
        memset(dtk, 0, frames * 2 * sizeof(int32_t));
        const size_t n = g_dtk.mix_into(dtk, frames);
        const int vl = g_dtk_vol_l.load(std::memory_order_relaxed);
        const int vr = g_dtk_vol_r.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; i++) {
            acc[i * 2 + 0] += dtk[i * 2 + 0] * vl >> 8;
            acc[i * 2 + 1] += dtk[i * 2 + 1] * vr >> 8;
        }
    }
    int16_t* out = (int16_t*)stream;
    const bool mute = g_muted.load(std::memory_order_relaxed);
    for (size_t i = 0; i < frames * 2; i++) {
        int32_t v = acc[i];
        v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        out[i] = mute ? 0 : (int16_t)v;
    }
    g_cb_count.fetch_add(1, std::memory_order_relaxed);
    if (dbg()) {
        static uint64_t last = 0;
        const uint64_t c = g_cb_count.load();
        if (c - last >= (uint64_t)kOutRate / (frames ? frames : 1)) {   // ~once per second
            last = c;
            fprintf(stderr, "[naudio] dsp_fill=%ldms dtk_fill=%ldms underruns dsp=%llu dtk=%llu\n",
                    g_dsp.fill_ms(), g_dtk.fill_ms(),
                    (unsigned long long)g_dsp.underruns.load(),
                    (unsigned long long)g_dtk.underruns.load());
        }
    }
}

}  // namespace

// ── native audio dump (SUNBRIGHT_DUMP_AUDIO) ───────────────────────────────────────────────
// The old dump rode on Dolphin's sound stream (dead under the null backend), so the sink owns
// it: push-side WAV writers, header patched at exit. Files land in scratch/wav/ (never /tmp).
namespace {
struct WavDump {
    FILE* f = nullptr;
    uint32_t rate = 0, bytes = 0;
    void open(const char* path, uint32_t r) {
        f = fopen(path, "wb");
        if (!f) return;
        rate = r;
        uint8_t hdr[44] = {};
        fwrite(hdr, 1, 44, f);                      // placeholder, patched in close()
    }
    void write(const int16_t* s, size_t frames) {
        if (!f) return;
        fwrite(s, 4, frames, f);
        bytes += (uint32_t)frames * 4;
    }
    void close() {
        if (!f) return;
        uint8_t h[44];
        const uint32_t byterate = rate * 4, riff = 36 + bytes;
        memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8);
        const uint32_t fmtsz = 16; const uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
        memcpy(h + 16, &fmtsz, 4); memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2);
        memcpy(h + 24, &rate, 4); memcpy(h + 28, &byterate, 4);
        memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
        memcpy(h + 36, "data", 4); memcpy(h + 40, &bytes, 4);
        fseek(f, 0, SEEK_SET);
        fwrite(h, 1, 44, f);
        fclose(f);
        f = nullptr;
    }
};
WavDump g_dump_dsp, g_dump_dtk;
bool dump_enabled() {
    static const bool on = [] {
        if (!getenv("SUNBRIGHT_DUMP_AUDIO")) return false;
        system("mkdir -p scratch/wav");
        atexit([] { g_dump_dsp.close(); g_dump_dtk.close(); });
        return true;
    }();
    return on;
}
}  // namespace

// ── producer-side entry points (called from the Mixer wraps / main_sdl) ─────────────────────
// The wraps intercept Mixer::Push*Samples BEFORE Dolphin's conversion: the AI DMA buffer is
// BIG-ENDIAN, R-L ordered stereo (see Mixer.cpp PushSamples: swap16(ptr[1]), swap16(ptr[0])).
// Convert to host-endian L-R here, so the ring AND the WAV dump carry playable PCM. Feeding
// the raw bytes through was the "complete garbage audio" bug — byteswapped PCM is loud noise,
// and its RMS looks healthy, so the WAV-RMS check could not catch it.
namespace {
size_t be_rl_to_host_lr(const int16_t* s, size_t n, int16_t* out, size_t out_cap_frames) {
    if (n > out_cap_frames) n = out_cap_frames;
    for (size_t i = 0; i < n; i++) {
        const uint16_t r = (uint16_t)s[i * 2 + 0], l = (uint16_t)s[i * 2 + 1];
        out[i * 2 + 0] = (int16_t)((l >> 8) | (l << 8));
        out[i * 2 + 1] = (int16_t)((r >> 8) | (r << 8));
    }
    return n;
}
}  // namespace
// Set on the first AUDIBLE DSP push (any nonzero sample): before this, the game has produced
// only silence and there is nothing for the time governor or the frame pacer to protect —
// boot/loading runs uncapped, PC-game style. (The DAC pushes all-zero frames from audio-init
// onward, seconds before the first real sound — "ever pushed at all" latches far too early.)
std::atomic<bool> g_na_ever_pushed{false};
static const auto g_na_t0 = std::chrono::steady_clock::now();   // ≈ process start (static init)
extern "C" bool na_ever_pushed() { return g_na_ever_pushed.load(std::memory_order_relaxed); }
extern "C" void na_push_dsp(const int16_t* s, size_t n) {
    if (!g_na_ever_pushed.load(std::memory_order_relaxed)) {
        for (size_t i = 0; i < n * 2; i++)
            if (s[i]) {
                g_na_ever_pushed.store(true, std::memory_order_relaxed);
                fprintf(stderr, "[naudio] first audible sample at host +%.1fs (boot pacing engages)\n",
                        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                      g_na_t0).count());
                break;
            }
    }
    if (!g_dsp.in_rate.load(std::memory_order_relaxed))
        g_dsp.in_rate.store(32028, std::memory_order_relaxed);   // default until divisor seen
    static int16_t conv[kRingCap * 2];
    n = be_rl_to_host_lr(s, n, conv, kRingCap);
    g_dsp.push(conv, n);
    if (dump_enabled()) {
        if (!g_dump_dsp.f && !g_dump_dsp.rate)
            g_dump_dsp.open("scratch/wav/native_dsp.wav", g_dsp.in_rate.load());
        g_dump_dsp.write(conv, n);
    }
}
extern "C" void na_push_dtk(const int16_t* s, size_t n) {
    if (!g_dtk.in_rate.load(std::memory_order_relaxed))
        g_dtk.in_rate.store(48043, std::memory_order_relaxed);
    static int16_t conv[kRingCap * 2];
    n = be_rl_to_host_lr(s, n, conv, kRingCap);
    g_dtk.push(conv, n);
    if (dump_enabled()) {
        if (!g_dump_dtk.f && !g_dump_dtk.rate)
            g_dump_dtk.open("scratch/wav/native_dtk.wav", g_dtk.in_rate.load());
        g_dump_dtk.write(conv, n);
    }
}
extern "C" void na_set_dsp_rate(uint32_t rate) { g_dsp.in_rate.store(rate, std::memory_order_relaxed); }
extern "C" void na_set_dtk_rate(uint32_t rate) { g_dtk.in_rate.store(rate, std::memory_order_relaxed); }
extern "C" void na_set_dtk_volume(int l, int r) {
    g_dtk_vol_l.store(l, std::memory_order_relaxed);
    g_dtk_vol_r.store(r, std::memory_order_relaxed);
}

// Governor audio-clock signal: DSP ring fill in ms (LONG_MIN until the stream starts). The
// sink consumes at the device clock; emulated time leads the host clock while this is low.
extern "C" long sunbright_audio_fill_ms() {
    if (!g_dev) return LONG_MIN;                       // no native sink: no audio clock
    return g_dsp.fill_ms();
}

extern "C" bool na_start(bool muted) {
    g_muted.store(muted, std::memory_order_relaxed);
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[naudio] SDL audio init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want{}, got{};
    want.freq = kOutRate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;                                // ~10.7 ms callback period
    want.callback = sdl_callback;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
    if (!g_dev) {
        fprintf(stderr, "[naudio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(g_dev, 0);
    fprintf(stderr, "[naudio] native sink up: %d Hz, %d-frame callbacks%s\n",
            got.freq, got.samples, muted ? " (muted)" : "");
    return true;
}
