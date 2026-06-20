// audio_seam.h — native reimplementation of the GC audio HW APIs (AI / DSP / AR).
//
// SDK headers replaced: reference/sms/include/dolphin/ai.h, dsp.h, ar.h
// Used surface: AI 16 distinct / 66 calls, DSP 11 / 104, AR ~10 real / ~29 calls.
// Hot: DSPCheckMailToDSP(37), DSPSendMailToDSP(36), AISetStreamVolL/R(11 each),
//      ARStartDMA(7), ARAlloc(7).
//
// SMS does NOT use the AX library — it drives the DSP microcode (JAudio2) directly
// via the DSP mailbox (osdsp.c / dspproc.c). On the native port the DSP microcode
// is REPLACED, not emulated: native_jas synthesizes voices on the host CPU and
// writes PCM to a host audio device. Therefore:
//
//   AI  -> host audio device. AIInitDMA / AIRegisterDMACallback model the audio
//          ring-buffer refill (the DMA callback = "device wants more samples"; it
//          is the audio clock). AISetStream* / AIGetStream* control the DTK ADPCM
//          stream (volume/state/rate) -> route to the stream mixer.
//   DSP -> INERT. The mailbox calls (DSPSendMailToDSP/DSPCheckMailToDSP/DSPAddTask/
//          DSPInit) ack / no-op benignly so the engine's DSP-task bookkeeping runs
//          without a real ucode. native_jas does the synthesis instead.
//   AR  -> a plain native heap. ARAM was 16 MB aux RAM for wave-bank staging;
//          ARAlloc = bump-alloc in a malloc'd block, ARStartDMA = memcpy. native_jas
//          decodes waves straight from the ROM, so ARAM staging is mostly vestigial.
//
// PRIOR ART: runtime/native_jas.cpp (the native JAS engine — WSYS/IBNK/sequence
// decode + AFC voice synth + mix) and runtime/native_audio.cpp (the SDL 48 kHz
// device that is the audio clock, with Mixer::Push* wraps + fill servo).
#pragma once
#include "platform_types.h"

namespace sb::platform::audio {

// ---- host device + native_jas bring-up ----------------------------------
bool Init();      // open the host audio device (48 kHz stereo), start native_jas
void Shutdown();

// ---- AI (ai.h) ----------------------------------------------------------
using DmaCallback = void (*)();
void  AIInit();                                  // AIInit (TODO)
DmaCallback RegisterDMACallback(DmaCallback cb); // AIRegisterDMACallback (TODO: refill servo)
void  InitDMA(u32 startAddr, u32 length);        // AIInitDMA (TODO)
void  StartDMA();   void StopDMA();              // AIStartDMA/StopDMA (TODO)
void  SetDSPSampleRate(u32 rate);                // AISetDSPSampleRate (TODO)
// DTK ADPCM stream control (AISetStream*/AIGetStream*):
void  SetStreamVolLeft(u32 v);  void SetStreamVolRight(u32 v);   // TODO
void  SetStreamPlayState(u32 s); u32 GetStreamPlayState();       // TODO
void  SetStreamSampleRate(u32 r);                                // TODO

// ---- DSP (dsp.h) — inert mailbox; native_jas does the work --------------
void  DSPInit();                                 // DSPInit (TODO: no-op-ish)
u32   SendMailToDSP(u32 mail);                   // DSPSendMailToDSP (TODO: ack)
bool  CheckMailToDSP();                          // DSPCheckMailToDSP (TODO: ready)
bool  CheckMailFromDSP();   u32 ReadMailFromDSP();  // TODO
void* AddTask(void* task);                       // DSPAddTask (TODO: bookkeeping)

// ---- AR / ARAM (ar.h) — native heap -------------------------------------
u32   ARInit();                                  // ARInit (TODO)
u32   ARAlloc(u32 length);                       // ARAlloc (TODO: bump-alloc)
void  ARStartDMA(u32 type, u32 mainmemAddr, u32 aramAddr, u32 length); // ARStartDMA -> memcpy (TODO)
u32   ARGetSize();   u32 ARGetBaseAddress();     // TODO

} // namespace sb::platform::audio
