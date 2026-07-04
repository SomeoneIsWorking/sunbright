// pad_input.h — host-input feed for the PAD seam (native/platform/pad_impl.cpp).
//
// The integration layer (host gamepad/keyboard polling, sampled on the VI retrace)
// pushes raw controller state per channel via sb_pad_set_state; PADRead copies it
// into the game's PADStatus array. Keeps the seam GameCube-free and host-agnostic.
#pragma once
#include <dolphin/pad.h>

extern "C" {
// Feed channel `chan` (0..3) with a raw PADStatus snapshot. Set status->err to
// PAD_ERR_NONE for a connected pad, PAD_ERR_NO_CONTROLLER for an absent one.
void sb_pad_set_state(int chan, const PADStatus* status);
// Mark a channel absent (PADRead reports PAD_ERR_NO_CONTROLLER).
void sb_pad_set_absent(int chan);
}
