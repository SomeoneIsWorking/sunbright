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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
FILE* out() {
    static FILE* f = [] {
        const char* p = getenv("SUNBRIGHT_DUMP_VPB");
        return p ? fopen(p, "w") : nullptr;
    }();
    return f;
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
