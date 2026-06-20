// pad_seam.h — native reimplementation of the GC PAD + SI (input) APIs.
//
// SDK headers replaced: reference/sms/include/dolphin/pad.h, si.h
// Used surface: PAD 17 distinct / 58 calls, SI 22 / 75. Hot: PADControlMotor(10)
// (rumble), PADReset(5), PAD{En,Dis}able(5), PADRead(3), PADClamp, PADRecalibrate;
// SI is the serial transport UNDER PAD (SITransfer(12), SIGetType(9), polling).
//
// MAPPING (see README.md "PAD"):
//   A host gamepad (or keyboard) fills a PADStatus per channel. SI is folded in: it
//   is just the GC serial bus PAD rides on, so the SI seam reports "standard
//   controller present" on channel 0 and the PAD seam reads host input into the
//   SDK PADStatus layout. PADControlMotor -> host rumble. PADClamp/Recalibrate ->
//   the SDK's stick dead-zone/origin math (pure, keep it). Sampling callbacks fire
//   once per VI retrace (see vi_seam) so the engine's input cadence is preserved.
//
// PADStatus keeps its SDK layout (button bitmask, stickX/Y, substickX/Y, triggers,
// err). Map host buttons -> the GC button bits the game reads.
#pragma once
#include "platform_types.h"

namespace sb::platform::pad {

struct Status;   // opaque, SDK PADStatus layout (per channel)
using SamplingCallback = void (*)();

// ---- bring-up (pad.h) ---------------------------------------------------
bool Init();                          // PADInit (TODO: open host gamepads)
u32  Reset(u32 mask);                 // PADReset (TODO)
bool Recalibrate(u32 mask);           // PADRecalibrate (TODO)

// ---- per-frame read -----------------------------------------------------
u32  Read(Status* statusArray);       // PADRead -> fill 4 channels from host input (TODO)
void Clamp(Status* statusArray);      // PADClamp -> SDK dead-zone/clamp math (TODO, pure)
void ControlMotor(s32 chan, u32 command);  // PADControlMotor -> host rumble (TODO)
void Enable(u32 mask);   void Disable(u32 mask);   // PADEnable/Disable (TODO)
void SetSamplingCallback(SamplingCallback cb);     // fires per VI retrace (TODO)
void SetSpec(u32 spec);  u32 GetSpec();            // PADSetSpec/GetSpec (TODO)

// ---- SI transport (si.h) — folded in; mostly trivial natively -----------
// SIGetType -> "standard controller" on chan 0 (and configured extra pads). SITransfer
// / polling handlers are the GC bus mechanics; the native seam services them as
// no-op/inline so any direct SI caller links. The engine reads input via PAD, so SI
// rarely needs more than type reporting. TODO phase-2.
u32  SIGetType(s32 chan);   // TODO

} // namespace sb::platform::pad
