// interp60_replace.h — RECORD-AND-REPLACE frame interpolation.
//
// See interp60_replace.cpp for the design and for what this does NOT cover.

#pragma once

#include "cpu_state.h"

// SBR_INTERP60_REPLACE=1. When off, every entry point below is a no-op and the sub-frame keeps the
// substitute-and-re-issue path.
bool sbr_i60r_enabled();

// One guest tick has begun: the recording that was "current" becomes "previous".
void sbr_i60r_begin_tick();

// A J3DModel has just finished its REAL viewCalc, so mDrawMtxBuf[1][viewNo] holds this tick's final
// draw matrices. Records them keyed by (model, index). Must not be called inside a sub-frame.
void sbr_i60r_record(u32 model);

// Write lerp(prev, cur, alpha) into every model's live draw-matrix buffer, saving the originals.
// Returns the number of models replaced. Call immediately before the sub-frame's draw lists.
int sbr_i60r_apply(float alpha);

// Put the saved originals back, byte-exactly, and check whether anything else wrote to the buffers
// while the sub-frame was drawing. Call immediately after the sub-frame's draw lists.
void sbr_i60r_restore();

// Periodic stats line, with denominators.
void sbr_i60r_report();
