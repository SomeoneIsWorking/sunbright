// gx_blend_eval.h — pure GX pixel-blend equation evaluator (float, clamped).
//
// The GX per-pixel blend runs AFTER the TEV combiner and BEFORE writeback:
//   BM_BLEND:    out.rgb = src.rgb * src_factor + dst.rgb * dst_factor
//   BM_SUBTRACT: out.rgb = dst.rgb * dst_factor - src.rgb * src_factor
// Each factor is a 3-bit index (GXBlendFactor 0..7). All channels are clamped
// to [0, 1] after combining. Alpha writeback is gated separately (mAlphaUpdate).
//
// This header is the single-source-of-truth for the arithmetic that
//   * runtime/gxblend_summary.h names in text (formatter, not evaluator)
//   * native/render/gx_sdlgpu.cpp translates to SDL_GPUBlendFactor
//   * the SDL3 GPU pipeline computes per fragment
// and it is unit-tested Dolphin-free so the shipping blend cannot silently
// deviate from GX spec.
//
// The tests also lock a counterintuitive discriminator used for the file-
// select overbright investigation: SRCALPHA/SRCCLR with src=white/alpha=1
// SATURATES to (1,1,1) in a single pass regardless of dst — so the observed
// native wash (~(220,230,229) over base turquoise (102,183,189)) CANNOT be
// explained by "the eb5c8e74 mask packet drawn once or twice" (that would
// predict full 255 saturation). The wash driver must be found elsewhere.
// The pure test captures that discriminator numerically so future overbright
// arcs don't waste a session re-deriving it.

#pragma once
#include <cstdint>

namespace sb::gxblend {

// GX blend-factor indices (GXBlendFactor).
enum : uint8_t {
    ZERO       = 0,
    ONE        = 1,
    SRCCLR     = 2,   // dst_only: fetches src.rgb — hence "same as src" for the dst term
    INVSRCCLR  = 3,   // 1 - src.rgb (per-channel)
    SRCALPHA   = 4,   // src.a
    INVSRCALPHA= 5,   // 1 - src.a
    DSTALPHA   = 6,   // dst.a
    INVDSTALPHA= 7,   // 1 - dst.a
};

struct Rgba { float r, g, b, a; };

inline float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

// Fetch the multiplicative factor for one channel `ch` (0=R, 1=G, 2=B). The SRCCLR /
// INVSRCCLR / SRCALPHA / INVSRCALPHA / DSTALPHA / INVDSTALPHA factors are all POSITIVE
// (never clamped negative — they are inputs to the blend, not the result), but they can
// exceed 1.0 in intermediate computation for INV* if src/dst is out-of-range (won't happen
// in valid GX state — src/dst always fed clamped from TEV / previous framebuffer).
inline float factor_value(uint8_t f, const Rgba& src, const Rgba& dst, int ch) {
    const float sc = (ch == 0) ? src.r : (ch == 1) ? src.g : src.b;
    const float dc = (ch == 0) ? dst.r : (ch == 1) ? dst.g : dst.b;
    switch (f) {
        case ZERO:        return 0.0f;
        case ONE:         return 1.0f;
        case SRCCLR:      return sc;
        case INVSRCCLR:   return 1.0f - sc;
        case SRCALPHA:    return src.a;
        case INVSRCALPHA: return 1.0f - src.a;
        case DSTALPHA:    return dst.a;
        case INVDSTALPHA: return 1.0f - dst.a;
    }
    return 0.0f;
}

// Apply the GX pixel blend. `subtract=false` = GX_BM_BLEND, `subtract=true` = GX_BM_SUBTRACT.
// Alpha channel is NOT written by the RGB blend equation (GX alphaBlend is a SEPARATE
// path — see GXSetDstAlpha); the returned Rgba's .a is a passthrough of `dst.a` so
// consecutive blend() calls chain the dst alpha through unchanged (matches the actual GX
// pipeline when alpha_update is off — the common case for the overbright mask packets).
inline Rgba blend(uint8_t src_factor, uint8_t dst_factor, bool subtract,
                  const Rgba& src, const Rgba& dst) {
    Rgba out{};
    for (int ch = 0; ch < 3; ++ch) {
        const float sf = factor_value(src_factor, src, dst, ch);
        const float df = factor_value(dst_factor, src, dst, ch);
        const float s  = (ch == 0) ? src.r : (ch == 1) ? src.g : src.b;
        const float d  = (ch == 0) ? dst.r : (ch == 1) ? dst.g : dst.b;
        const float v  = subtract ? (d * df - s * sf) : (s * sf + d * df);
        float& outC    = (ch == 0) ? out.r : (ch == 1) ? out.g : out.b;
        outC = clamp01(v);
    }
    out.a = dst.a;
    return out;
}

// Apply N successive blends of the SAME src over an evolving dst — the "double flush"
// / "N-pass composite" experiment. Each pass reads the previous pass's output as its dst,
// which is what happens when the same DrawBuf is flushed multiple times per frame.
inline Rgba blend_n(int n, uint8_t src_factor, uint8_t dst_factor, bool subtract,
                    const Rgba& src, Rgba dst) {
    for (int i = 0; i < n; ++i) dst = blend(src_factor, dst_factor, subtract, src, dst);
    return dst;
}

}  // namespace sb::gxblend
