#pragma once
// Per-epoch offscreen CONTENT selection (pure; unit-tested in render_test `per_epoch`).
//
// Some GXCopyTex copies grab an OFFSCREEN epoch — geometry the game draws into the EFB, then copies
// into a texture the displayed scene samples. The motivating case is the Sirena "graffito check":
// the "Manta Storm" goo mask shapes are drawn, then a GX_CTF_R8 (fmt 0x28) GXCopyTex grabs the EFB
// RED channel into a per-layer R8 COVERAGE texture (TPollutionLayer.unk54). The display-epoch
// pollution-layer plane samples that coverage → the goo appears. Under ngx present Dolphin's EFB is
// empty, so that copy grabs black ⇒ no coverage ⇒ invisible goo.
//
// The present fixes it by RE-RENDERING just that copy's epoch (batch.epoch == event.epoch) into the
// copy's texture and storing it in ngx's EFB side buffer keyed by the dst EA, so texture_for serves
// it. These pure predicates decide WHICH events get per-epoch content and WHICH batches feed them,
// and convert a rendered pixel to the R8 coverage scalar GX_CTF_R8 would have grabbed.
#include <cstdint>
#include "ngx_render_data.h"   // NgxCopyEvent

namespace ngx_perepoch {

// GX_CTF_R8 copy format (copy the EFB red component to an 8-bit texture). Bound later as I8/R8 →
// sampled (I,I,I,I).
constexpr int CTF_R8 = 0x28;

// Should the present render per-epoch offscreen CONTENT for this copy event? Only the R8 coverage
// (graffito) copy for now: offscreen (kind 0), R8 format, valid dest + dims. The standard COLOUR
// copy formats (< 0x10, e.g. RGB565 reflections) are already served from the display-scene readback
// by copytex_writeback; display copies (kind 1) are the on-screen frame, not offscreen content.
inline bool wants_offscreen_content(const NgxCopyEvent& e) {
    return e.kind == 0 && e.dest != 0 && e.dst_w > 0 && e.dst_h > 0 && e.fmt == CTF_R8;
}

// Does a batch drawn under `batch_epoch` belong to this copy event's source epoch (i.e. it is part
// of the geometry the copy captures)?
inline bool batch_in_copy(uint16_t batch_epoch, const NgxCopyEvent& e) {
    return batch_epoch == e.epoch;
}

// Rendered EFB pixel (ARGB8888) → R8 coverage texel GX_CTF_R8 grabs (the RED channel), replicated to
// all four channels so the later I8/R8 sample expands to (cov,cov,cov,cov) — matching how the
// pollution-layer plane reads its coverage.
inline uint32_t argb_to_r8_coverage(uint32_t argb) {
    const uint32_t r = (argb >> 16) & 0xFFu;
    return (r << 24) | (r << 16) | (r << 8) | r;
}

}  // namespace ngx_perepoch
