#pragma once
// gxtev_summary.h — pure, Dolphin-free decoder for the per-STAGE GX TEV combiner.
//
// The per-draw BLEND value oracle (gxblend_summary.h) tells us a draw's blend equation and stage
// COUNT, but the file-select sea-water overbright (b76) needs the combiner CONTENTS: the native
// renderer paints the sea opaque-white, and the question is whether native's generated TEV
// (sms_boot_material → tev_shader) faithfully matches what the GameCube programmed. To settle that by
// VALUE we read Dolphin's BPMEM_TEV_COLOR_ENV/ALPHA_ENV (0xC0..0xDF) from the command stream and decode
// each active stage's a/b/c/d inputs, bias, op, scale, clamp and dest — the SAME bit layout the native
// renderer stores in NgxTevStage::color_env/alpha_env, so the two are diffed register-for-register.
//
// Kept pure (no Dolphin headers) so it is unit-tested in render_test and is the SAME code gx_capture.cpp
// ships (the test validates the shipping path, not a fork). The GX bit layout mirrors
// VideoCommon/BPMemory.h TevStageCombiner::ColorCombiner / AlphaCombiner.

#include <cstdint>
#include <string>
#include <cstdio>

namespace gxtev {

// One TEV stage = the two 24-bit GX combiner register words (identical to NgxTevStage / Dolphin).
struct Stage {
    uint32_t color_env = 0;   // ColorCombiner: d0-3 c4-7 b8-11 a12-15 bias16-17 op18 clamp19 scale20-21 dest22-23
    uint32_t alpha_env = 0;   // AlphaCombiner: rswap0-1 tswap2-3 d4-6 c7-9 b10-12 a13-15 bias16-17 op18 clamp19 scale20-21 dest22-23
};

// GX TevColorArg (0..15) names — match Dolphin's ColorCombiner formatter vocabulary.
inline const char* color_arg(uint8_t a) {
    static const char* k[16] = {"prev.rgb","prev.a","c0.rgb","c0.a","c1.rgb","c1.a","c2.rgb","c2.a",
                                "tex.rgb","tex.a","ras.rgb","ras.a","ONE","HALF","konst","ZERO"};
    return k[a & 15];
}
// GX TevAlphaArg (0..7) names.
inline const char* alpha_arg(uint8_t a) {
    static const char* k[8] = {"prev","c0","c1","c2","tex","ras","konst","ZERO"};
    return k[a & 7];
}
inline const char* bias_name(uint8_t b)  { static const char* k[4] = {"0","+.5","-.5","comp"}; return k[b & 3]; }
inline const char* scale_name(uint8_t s) { static const char* k[4] = {"x1","x2","x4","x.5"};  return k[s & 3]; }
inline const char* dest_name(uint8_t d)  { static const char* k[4] = {"prev","c0","c1","c2"};  return k[d & 3]; }

// Decode one stage's COLOR combiner into a readable line: `Cd=.. a=.. b=.. c=.. op=+ bias=0 sc=x2 clamp dst=prev`.
inline std::string format_color(uint32_t ce) {
    const uint8_t d = ce & 15, c = (ce >> 4) & 15, b = (ce >> 8) & 15, a = (ce >> 12) & 15;
    const uint8_t bias = (ce >> 16) & 3, op = (ce >> 18) & 1, clamp = (ce >> 19) & 1,
                  scale = (ce >> 20) & 3, dest = (ce >> 22) & 3;
    char buf[200];
    std::snprintf(buf, sizeof buf, "C d=%s a=%s b=%s c=%s op=%c bias=%s sc=%s%s dst=%s",
                  color_arg(d), color_arg(a), color_arg(b), color_arg(c), op ? '-' : '+',
                  bias_name(bias), scale_name(scale), clamp ? " clamp" : "", dest_name(dest));
    return std::string(buf);
}
// Decode one stage's ALPHA combiner.
inline std::string format_alpha(uint32_t ae) {
    const uint8_t d = (ae >> 4) & 7, c = (ae >> 7) & 7, b = (ae >> 10) & 7, a = (ae >> 13) & 7;
    const uint8_t bias = (ae >> 16) & 3, op = (ae >> 18) & 1, clamp = (ae >> 19) & 1,
                  scale = (ae >> 20) & 3, dest = (ae >> 22) & 3;
    const uint8_t rswap = ae & 3, tswap = (ae >> 2) & 3;
    char buf[200];
    std::snprintf(buf, sizeof buf, "A d=%s a=%s b=%s c=%s op=%c bias=%s sc=%s%s dst=%s rsw=%u tsw=%u",
                  alpha_arg(d), alpha_arg(a), alpha_arg(b), alpha_arg(c), op ? '-' : '+',
                  bias_name(bias), scale_name(scale), clamp ? " clamp" : "", dest_name(dest), rswap, tswap);
    return std::string(buf);
}

}  // namespace gxtev
