#pragma once
// state_oracle — per-DRAW state comparison between this port's FIFO parse and aurora's.
//
// Why this exists. Aurora renders Delfino correctly from the SAME command stream, in the SAME
// process, reading the SAME guest memory. So when the native path draws something else, the
// difference is in the STATE each side derived, and that difference is directly observable — no
// oracle capture, no pixel metric, no hypothesis. A whole-frame score can say something is wrong
// but never WHERE; this says which draw, which field, and what each side thinks it is.
//
// Both sides walk the same stream in the same order (this parser first, aurora on the forwarded
// bytes), so the Nth draw on one side is the Nth draw on the other.
//
//   SBR_STATE_DIFF=<n>   compare, and report the first n disagreeing draws of a frame.

#include <cstdint>

// One draw's texture/TEV state, in GX's own terms so both sides can fill it without translation.
struct SbrDrawState {
    // Byte offset of this draw's command in the frame's FIFO stream. THE pairing key: aurora
    // replays exactly the buffer this parser emitted, so its cmdPos is this offset. Pairing by
    // ordinal instead drifts through a frame (aurora records ~24 fewer draws per frame) and
    // silently compares unrelated draws — which is how a state oracle manufactures findings.
    uint32_t pos = 0;
    uint8_t  numStages = 0;
    uint8_t  numTexGens = 0;
    // Per stage: the map it names, the coordinate it names, and whether the texture is enabled.
    uint8_t  texmap[16] = {};
    uint8_t  texcoord[16] = {};
    uint8_t  texEnable[16] = {};
    // Per unit: the texture identity. On this side that is the TX_SETIMAGE3 address; on aurora's
    // it is texObjId, which this port sets to exactly that address — so they compare directly.
    uint32_t unitId[8] = {};

    // ---- The rest of the pixel-deciding state, beyond texture identity. The stage/texture block
    // above was proven to agree at 99.55% of draws, yet the frames differ — so the disagreement,
    // if it is in the STATE at all, has to be in one of these fields. Everything is kept in raw
    // hardware encoding so both sides fill it without interpretation.
    //
    // Colour channels, one packed word per channel (colour0, colour1, alpha0, alpha1):
    //   bit0 matSrcVtx | bit1 lightingEnabled | bit2 ambSrcVtx | bits3-4 diffuseFn |
    //   bits5-6 attnFn (0 none, 1 spec, 2 spot) | bits8-15 lightMask
    uint16_t chanCtrl[4] = {};
    uint32_t ambColor[2] = {};   // RGBA8 (XF 0x100A/0x100B); the alpha channels share the register
    uint32_t matColor[2] = {};   // RGBA8 (XF 0x100C/0x100D)
    uint8_t  numChans = 0;
    // Per stage: the rasterised channel it reads, CANONICALISED to {0=colour0a0, 1=colour1a1,
    // 5=alphabump, 6=alphabumpN, 7=zero} — the hardware raw values 2/3/4 alias 0/1 and aurora
    // stores only the canonical form, so both sides canonicalise before comparing.
    uint8_t  rasChannel[16] = {};
    // Per stage: the raw 24-bit combiner words (colour; alpha with the swap-select bits 0-3
    // zeroed, this port does not model swap tables) and the konst selectors.
    uint32_t cWord[16] = {};
    uint32_t aWord[16] = {};
    uint16_t kSel[16] = {};      // kC | kA<<8
    uint32_t konst[4] = {};      // RGBA8
    uint64_t tevReg[4] = {};     // 4 x u16-wrapped signed S10 raw (r,g,b,a high..low)
    // Texgen types (raw XF: 2 = SRTG colour0, 3 = SRTG colour1) — filled only on this port's side
    // and NOT compared; used to know which colour channels a draw actually consumes.
    uint8_t  tgType[8] = {};
};

// Forward decls for the shared filler below.
struct SbrTevState;
struct SbrXfState;

// Fill the stage table and the pixel-state block from this port's parsed state. Shared by the
// FIFO-draw record and the capture seam so the two cannot pack the same state differently.
// pos/unitId stay with the caller.
void sbr_draw_state_fill(SbrDrawState& s, const SbrTevState& tev, const SbrXfState& xf);

extern "C" bool sbr_state_diff_enabled();

// Called once per draw command, by each side, in stream order.
void sbr_state_oracle_mine(const SbrDrawState& s);
void sbr_state_oracle_aurora(const SbrDrawState& s);

// Frame boundaries. Each side closes its own; the report pairs the oldest closed frame from each.
void sbr_state_oracle_mine_frame_end();
extern "C" void sbr_state_oracle_aurora_frame_end();

// THE THIRD CONSUMER. The renderer does not read the FIFO state directly: j3d_capture.cpp
// snapshots it at J3DShape::draw, on the CPU side. The oracle proves the FIFO state is right, which
// says nothing about whether the SNAPSHOT is attached to the draw it describes. `pos` is the
// parser's stream offset when the snapshot was taken; the report correlates it with the first FIFO
// draw at or after that offset — the drawable's own first draw — and they must be identical.
void sbr_state_oracle_capture(uint32_t pos, const SbrDrawState& s);

// Compare what both sides recorded and report the first disagreements; clears for the next frame.
void sbr_state_oracle_report();
