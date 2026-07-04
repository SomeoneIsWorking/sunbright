// Pollution-coverage feedback — the PURE per-texel core of TPollutionCounterLayer::countTexDegree.
//
// The Sirena "Manta Storm" goo is the TPollutionLayer J3D plane, masked by a per-layer I8 512×512
// coverage texture (`unk54`). On hardware that coverage is a GPU feedback render-to-texture: each
// frame countTexDegree clears, re-draws the PREVIOUS coverage through a per-texel TEV (the feedback),
// then applies stamps, and a GX_CTF_R8 EFB-copy writes the result back to `unk54`. Under ngx present
// Dolphin's EFB is discarded, so the coverage never lands → invisible goo. We reproduce the feedback
// NATIVELY (this header) into a host buffer that ngx samples as the goo mask.
//
// This file is the PURE, GPU-free, spec-derived feedback math + the coverage tiling map, extracted so
// render_test can assert it against hand-computed truth. The shipping override (pollution_coverage_native.cpp)
// MUST call these — no forked copy.
//
// Feedback TEV (reverse-engineered from initGXforPollutionLayer, PollutionCount.cpp, + Dolphin TEV
// semantics GX_TEV_ADD/SUB/COMP_R8_GT). C0 = TEVREG0 (= layer->unk84), C1 = TEVREG1 (= layer->unk85),
// `type` = layer->unk30, `flags` = layer->unk32. The branch matches initGXforPollutionLayer exactly:
//   type == 7        : GROW   new = prev + (prev > C1 ? C0 : 0)         [2-stage COMP_R8_GT then ADD]
//   flags & 2        : DECAY  new = prev - 2                            [1-stage SUB, C0 forced to 2]
//   else             : DECAY  new = prev - (C1 > prev ? C0 : 0)         [COMP_R8_GT then SUB; erodes <C1]
// All clamped to [0,255]. GX_TEV_COMP_R8_GT(a,b,c,d) = d + ((a.r > b.r) ? c : 0); GX_TEV_SUB(a,b,c,d)
// = d - lerp(a,b,c); GX_TEV_ADD(a,b,c,d) = d + lerp(a,b,c). The intensity channels are all equal
// (I8 → r==g==b==a), so we track one u8.
#ifndef SB_NGX_POLLUTION_H
#define SB_NGX_POLLUTION_H

#include <cstdint>

namespace sb_pollution {

// One feedback step on a single coverage texel. type=layer->unk30, flags=layer->unk32, c0/c1 the TEV
// registers. Returns the new coverage value (clamped 0..255).
inline uint8_t feedback_step(uint8_t prev, uint16_t type, uint16_t flags, uint8_t c0, uint8_t c1) {
    int v = prev;
    if (type == 7) {
        v = prev + (prev > c1 ? c0 : 0);            // GROW: hold + accrete above threshold
    } else if (flags & 2) {
        v = prev - 2;                               // DECAY by 2 (C0 forced to 2 in initGX)
    } else {
        v = prev - (c1 > prev ? c0 : 0);            // DECAY below threshold C1, hold ≥ C1
    }
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

// GC 8×4-block tiling index for an I8 texture of row-stride 1<<unk8 cols (TPollutionPos::index). Maps a
// logical (x,y) to the byte offset in the tiled image. The coverage `unk54` and the depth/area map use
// the SAME tiling, so this de-tiles both. (unk8 = log2(width); blocksPerRow = width>>3 = 1<<(unk8-3).)
inline uint32_t tiled_index(int x, int y, int unk8) {
    return (uint32_t)((y & 3) * 8 + (((x >> 3) + ((y >> 2) << (unk8 - 3))) * 0x20) + (x & 7));
}

}  // namespace sb_pollution

#endif  // SB_NGX_POLLUTION_H
