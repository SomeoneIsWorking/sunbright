// DSP HLE voice parameter-block tracer (linker --wrap on ZeldaAudioRenderer::FetchVPB).
//
// THE ear-free JAS-voice probe (2026-06-11): under recomp, JAS voices play exactly one 5 ms
// subframe then go silent, while pure Dolphin sustains them. The per-function diff harness shows
// no mistranslation, so the divergence is in the per-frame VOICE PARAMETERS the game writes for
// the DSP. This wrap logs, for every voice the HLE fetches each frame, the fields that decide
// audibility: enabled/done flags, the 6 channel target/current volumes, dolby volume, sample
// position. Diffing the oracle log against the recomp log names the dying parameter directly.
//
// Output: text lines to SUNBRIGHT_DUMP_VPB=<path>. Format:
//   <frame#> v<voice> en=<enabled> done=<done> ch=<id:tgt:cur>x6 dolby=<cur>/<tgt> pos=<hi:lo>
// Frame# increments on voice_id wrap (a new render pass fetches voices in order).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "Core/Core.h"
#include "Core/System.h"
#include "Core/HW/DSP.h"

extern uint32_t mem_r32(uint32_t ea);   // memory_bridge.cpp
extern uint16_t mem_r16(uint32_t ea);

namespace {
FILE* out() {
    static FILE* f = [] {
        const char* p = getenv("SUNBRIGHT_DUMP_VPB");
        return p ? fopen(p, "w") : nullptr;
    }();
    return f;
}

// ---- audio A/B harness event stream (docs/audio_ab_harness.md) -------------
// SUNBRIGHT_AB_EVENTS=<path>: JSONL voice start/end events with the cross-engine
// join key (FNV-1a of the first 64 ARAM bytes at the VPB base address — the same
// bytes the native engine hashes as Wave.srcHash).
FILE* ab_out() {
    static FILE* f = [] {
        const char* p = getenv("SUNBRIGHT_AB_EVENTS");
        return p ? fopen(p, "w") : nullptr;
    }();
    return f;
}
double ab_now_ms() {
    using namespace std::chrono;
    static const steady_clock::time_point t0 = steady_clock::now();
    return duration_cast<duration<double, std::milli>>(steady_clock::now() - t0).count();
}
struct AbVoice {
    bool on = false;
    uint32_t hash = 0;
    uint16_t ratio = 0;
    int peak = 0;
    double t_on = 0;
    unsigned frames = 0;
};
AbVoice g_ab_voice[0x60];
uint32_t aram_hash64(uint32_t base) {
    const uint8_t* p = Core::System::GetInstance().GetDSP().GetARAMPtr();
    if (!p) return 0;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < 64; i++) h = (h ^ p[base + i]) * 16777619u;
    return h;
}

// Oracle-side event source: an in-process poller over the guest's CH_BUF VPB table
// (0x8040E5B8 → 64 × 0x180-byte DSPBuffer, the same data the /vpb probe reads).
// NOTE: the FetchVPB --wrap above CANNOT serve this — all FetchVPB callers live in the
// same TU (Zelda.cpp), so the linker wrap never intercepts them (it only rewrites
// cross-object references). Enable edges persist a full 5 ms HLE frame, so a 1.5 ms
// poll catches every voice start/end. Enabled by SUNBRIGHT_AB_ORACLE=1 + AB_EVENTS.
struct AbOraclePoller {
    AbOraclePoller() {
        const char* e = getenv("SUNBRIGHT_AB_ORACLE");
        if (!e || !*e || *e == '0' || !getenv("SUNBRIGHT_AB_EVENTS")) return;
        std::thread([] {
            using namespace std::chrono_literals;
            FILE* ab = nullptr;
            while (!ab) { std::this_thread::sleep_for(250ms); ab = ab_out(); }
            // wait for the core (guest RAM) to come up
            while (Core::GetState(Core::System::GetInstance()) != Core::State::Running)
                std::this_thread::sleep_for(100ms);
            unsigned frames = 0;
            for (;;) {
                std::this_thread::sleep_for(1500us);
                const uint32_t base = mem_r32(0x8040E5B8u);
                if (base < 0x80000000u || base >= 0x81800000u) continue;
                frames++;
                for (int vi = 0; vi < 0x60; vi++) {
                    const uint32_t b = base + (uint32_t)vi * 0x180u;
                    const bool en = mem_r16(b) != 0 && mem_r16(b + 2) == 0;
                    AbVoice& v = g_ab_voice[vi];
                    if (!en && !v.on) continue;
                    int vol = 0;
                    if (mem_r16(b + 0x58)) {                       // use_dolby_volume
                        vol = (int16_t)mem_r16(b + 0x54); if (vol < 0) vol = -vol;
                    } else for (int c = 0; c < 6; c++) {
                        const uint32_t ch = b + 0x10 + (uint32_t)c * 8;
                        if (!mem_r16(ch)) continue;
                        int cv = (int16_t)mem_r16(ch + 4); if (cv < 0) cv = -cv;
                        if (cv > vol) vol = cv;
                    }
                    if (en && !v.on) {
                        const uint32_t wbase =
                            ((uint32_t)mem_r16(b + 0x118) << 16) | mem_r16(b + 0x11A);
                        v.on = true; v.hash = aram_hash64(wbase);
                        v.ratio = mem_r16(b + 4);
                        v.peak = vol; v.t_on = ab_now_ms(); v.frames = frames;
                        fprintf(ab, "{\"ev\":\"von\",\"t\":%.1f,\"voice\":%d,"
                                    "\"hash\":\"%08x\",\"ratio\":%u}\n",
                                v.t_on, vi, v.hash, v.ratio);
                        fflush(ab);
                    } else if (en) {
                        if (vol > v.peak) v.peak = vol;
                    } else {
                        v.on = false;
                        fprintf(ab, "{\"ev\":\"voff\",\"t\":%.1f,\"voice\":%d,"
                                    "\"hash\":\"%08x\",\"ratio\":%u,\"dur\":%u,"
                                    "\"peak\":%d}\n",
                                ab_now_ms(), vi, v.hash, v.ratio,
                                (unsigned)((ab_now_ms() - v.t_on) / 5.0), v.peak);
                        fflush(ab);
                    }
                }
            }
        }).detach();
    }
} g_ab_oracle_poller;
// VPB field offsets in u16 units (struct ZeldaAudioRenderer::VPB, Zelda.cpp:682).
constexpr int F_ENABLED = 0, F_DONE = 1;
constexpr int F_CH0 = 8;            // channels[6]: {id, target_volume, current_volume, unk} ×6
constexpr int F_DOLBY_CUR = 0x2A, F_DOLBY_TGT = 0x2B, F_USE_DOLBY = 0x2C;
constexpr int F_POS_H = 0x34, F_POS_L = 0x35;
}  // namespace

extern "C" void __real__ZN3DSP3HLE18ZeldaAudioRenderer8FetchVPBEtPNS1_3VPBE(
    void* self, uint16_t voice_id, void* vpb);
extern "C" void __wrap__ZN3DSP3HLE18ZeldaAudioRenderer8FetchVPBEtPNS1_3VPBE(
    void* self, uint16_t voice_id, void* vpb) {
    __real__ZN3DSP3HLE18ZeldaAudioRenderer8FetchVPBEtPNS1_3VPBE(self, voice_id, vpb);
    FILE* f = out();
    if (!f) return;
    const uint16_t* w = (const uint16_t*)vpb;
    static unsigned long frame = 0;
    static int last_voice = -1;
    if ((int)voice_id <= last_voice) frame++;          // ids restart each render pass
    last_voice = voice_id;
    if (!w[F_ENABLED] && !w[F_DONE]) return;           // idle slot: skip noise
    fprintf(f, "%lu v%u en=%u done=%u ratio=%04x src=%u base=%04x%04x", frame, voice_id,
            w[F_ENABLED], w[F_DONE], w[2], w[0x80], w[0x8C], w[0x8D]);
    for (int c = 0; c < 6; c++) {
        const uint16_t* ch = w + F_CH0 + c * 4;
        if (ch[0])  // active mixing channel
            fprintf(f, " ch%x=%d/%d", ch[0], (int16_t)ch[1], (int16_t)ch[2]);
    }
    if (w[F_USE_DOLBY])
        fprintf(f, " dolby=%d/%d", (int16_t)w[F_DOLBY_CUR], (int16_t)w[F_DOLBY_TGT]);
    fprintf(f, " pos=%u:%u\n", w[F_POS_H], w[F_POS_L]);
}
