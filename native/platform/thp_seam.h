// thp_seam.h — native reimplementation of the GC THP (FMV) player.
//
// SDK header replaced: reference/sms/include/dolphin/thp.h
// Used surface: 24 distinct / 66 calls. THPPlayerPlay(7), THPPlayerStop(6),
// THPPlayerPause(5), THPPlayerOpen/Close/Prepare/Init, THPPlayerDrawCurrentFrame,
// THPVideoDecode, THPAudioDecode, THPPlayerGetState/GetVideoInfo/GetAudioInfo.
//
// MAPPING (see README.md "THP"):
//   THP is Nintendo's video container: per-frame baseline-JPEG-like (DCT) video +
//   ADPCM audio, played from a disc file. Native seam = demux the THP, decode each
//   video frame natively, hand the RGB frame to the renderer (drawn as a fullscreen
//   quad through ngx) and the audio to the audio device. THPPlayer* run a decode
//   loop (typically its own thread). The recomp build already DCT-decodes THP (the
//   dcbz / FMV-comb fix). Reuse that decode math.
//
// THPPlayer state machine: Init -> Open(file) -> Prepare -> Play -> (per frame:
// DrawCurrentFrame + DrawDone, VideoDecode/AudioDecode advance) -> Stop/Pause -> Close.
#pragma once
#include "platform_types.h"

namespace sb::platform::thp {

enum class State : s32 { Stop, Play, Pause, Prepare };

// ---- bring-up + lifecycle (thp.h) ---------------------------------------
bool Init();                                    // THPInit / THPPlayerInit (TODO)
u32  CalcNeedMemory();                          // THPPlayerCalcNeedMemory (TODO)
bool SetBuffer(void* buffer);                   // THPPlayerSetBuffer (TODO)
bool Open(const char* fileName);                // THPPlayerOpen -> demux header (TODO)
bool Prepare(s32 frame, s32 flag, s32 audioTrack); // THPPlayerPrepare (TODO)
bool Play();   bool Stop();   bool Pause();      // THPPlayerPlay/Stop/Pause (TODO)
bool Close();                                   // THPPlayerClose (TODO)
State GetState();                               // THPPlayerGetState (TODO)

// ---- per-frame decode + present -----------------------------------------
void* DrawCurrentFrame(/*GX context*/);         // THPPlayerDrawCurrentFrame -> RGB frame -> ngx quad (TODO)
void  DrawDone();                               // THPPlayerDrawDone (TODO)
s32   VideoDecode(void* src, void* dstRGB);     // THPVideoDecode -> DCT decode one frame (TODO)
s32   AudioDecode(void* src, void* dstPCM);     // THPAudioDecode -> ADPCM decode (TODO)
void  GetVideoInfo(u32* w, u32* h, f32* fps);   // THPPlayerGetVideoInfo (TODO)
void  GetAudioInfo(u32* channels, u32* rate);   // THPPlayerGetAudioInfo (TODO)
void  SetVolume(s32 vol);                       // THPPlayerSetVolume (TODO)

} // namespace sb::platform::thp
