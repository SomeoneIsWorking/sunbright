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
    uint8_t  numStages = 0;
    uint8_t  numTexGens = 0;
    // Per stage: the map it names, the coordinate it names, and whether the texture is enabled.
    uint8_t  texmap[16] = {};
    uint8_t  texcoord[16] = {};
    uint8_t  texEnable[16] = {};
    // Per unit: the texture identity. On this side that is the TX_SETIMAGE3 address; on aurora's
    // it is texObjId, which this port sets to exactly that address — so they compare directly.
    uint32_t unitId[8] = {};
};

extern "C" bool sbr_state_diff_enabled();

// Called once per draw command, by each side, in stream order.
void sbr_state_oracle_mine(const SbrDrawState& s);
void sbr_state_oracle_aurora(const SbrDrawState& s);

// Frame boundaries. Each side closes its own; the report pairs the oldest closed frame from each.
void sbr_state_oracle_mine_frame_end();
extern "C" void sbr_state_oracle_aurora_frame_end();

// Compare what both sides recorded and report the first disagreements; clears for the next frame.
void sbr_state_oracle_report();
