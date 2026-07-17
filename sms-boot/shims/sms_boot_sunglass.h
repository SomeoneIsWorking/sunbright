// sms_boot_sunglass.h — pure, Dolphin-free, no-GX unit of the GC2D/SunGlass port.
//
// Extracts the parts of TSunGlass::perform (the alpha fade progression) that DON'T depend on GX
// or the game-pad. Lets a unit test hand-derive expected values from the RE (Ghidra decompile at
// scratch/decomp_sunglass/8017d264.c) without a ROM or GPU.
//
// The shipping decomp/sms/src/GC2D/SunGlass.cpp calls into these helpers so the test validates
// the SAME function the game uses.

#pragma once
#include <cstdint>

namespace sb {

// TSunGlass::perform's alpha-interpolation step. The RE at 0x8017d264 computes:
//     alpha_new = end + cur * (start - end) / total
// where cur/total are u16 step counters, start/end are u8 alphas. At cur=0 the alpha is END;
// at cur=total the alpha reaches START. (Yes — cur counts UP but alpha runs from end→start.
// That's what the disassembly shows, and startFade @0x8017d574 sets cur=0 at fade activation,
// so the visible fade begins at END and settles at START. Named here because it's not the
// intuitive direction and a "fix" that flipped it would be a real regression.)
//
// Arithmetic is done in float (matches the PPC u32→f64→f32 dance in the decompile), truncated
// back to a u8 alpha. Signed on (start-end) so a decreasing fade (start<end) works.
inline uint8_t sunglass_fade_step_alpha(uint8_t start, uint8_t end, uint16_t cur, uint16_t total) {
    if (total == 0) return end;  // avoid /0; disassembly assumes total>0 (startFade never sets 0)
    const float sf   = (float)((int)start - (int)end);
    const float f    = (float)end + (float)cur * sf / (float)total;
    // PPC truncation toward zero (int cast). Clamped to u8 range for safety — real code stores
    // through a byte write so it also wraps, but for a paletted alpha the semantic result matches.
    int i = (int)f;
    if (i < 0)   i = 0;
    if (i > 255) i = 255;
    return (uint8_t)i;
}

// TSunGlass::perform's step-count advance. If cur < total, increment cur; else deactivate.
// Returns the new (cur, active) pair. Named because the fade-complete semantics (cur reaches
// total → active goes false in the SAME frame) are subtle and a wrong branch would leave a
// completed fade continuing to redraw with the final alpha.
struct FadeAdvance { uint16_t new_cur; bool     new_active; };
inline FadeAdvance sunglass_fade_advance(uint16_t cur, uint16_t total) {
    if (cur < total) return { (uint16_t)(cur + 1), true };
    return { cur, false };
}

}  // namespace sb
