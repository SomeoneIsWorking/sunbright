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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Core/System.h"
#include "Core/HW/DSP.h"

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
    if (FILE* ab = ab_out(); ab && voice_id < 0x60) {
        const uint16_t* w = (const uint16_t*)vpb;
        AbVoice& v = g_ab_voice[voice_id];
        const bool en = w[0] != 0 && w[1] == 0;        // enabled && !done
        // live gain: dolby voices mix via dolby_volume_current; others via channels[6]
        int vol = 0;
        if (w[0x2C]) vol = (int16_t)w[0x2A] < 0 ? -(int16_t)w[0x2A] : (int16_t)w[0x2A];
        else for (int c = 0; c < 6; c++) {
            const uint16_t* ch = w + 8 + c * 4;
            const int cv = (int16_t)ch[2] < 0 ? -(int16_t)ch[2] : (int16_t)ch[2];
            if (ch[0] && cv > vol) vol = cv;
        }
        if (en && !v.on) {
            const uint32_t base = ((uint32_t)w[0x8C] << 16) | w[0x8D];
            v.on = true; v.hash = aram_hash64(base); v.ratio = w[2];
            v.peak = vol; v.t_on = ab_now_ms(); v.frames = 0;
            fprintf(ab, "{\"ev\":\"von\",\"t\":%.1f,\"voice\":%u,\"hash\":\"%08x\","
                        "\"ratio\":%u}\n", v.t_on, voice_id, v.hash, v.ratio);
            fflush(ab);
        } else if (v.on && en) {
            if (vol > v.peak) v.peak = vol;
            v.frames++;
        } else if (v.on && !en) {
            v.on = false;
            fprintf(ab, "{\"ev\":\"voff\",\"t\":%.1f,\"voice\":%u,\"hash\":\"%08x\","
                        "\"ratio\":%u,\"dur\":%u,\"peak\":%d}\n",
                    ab_now_ms(), voice_id, v.hash, v.ratio, v.frames, v.peak);
            fflush(ab);
        }
    }
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
