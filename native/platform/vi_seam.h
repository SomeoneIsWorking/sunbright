// vi_seam.h — native reimplementation of the GC VI (video interface) API.
//
// SDK headers replaced: reference/sms/include/dolphin/vi/*.h, vi.h
// Used surface: 14 distinct / 83 calls. Hot: VIGetTvFormat(17), VIWaitForRetrace(16),
// VIGetRetraceCount(8), VISetBlack(6), VIGetNextField(6), VIFlush(5), VIConfigure(3),
// VISet{Pre,Post}RetraceCallback, VISetNextFrameBuffer.
//
// MAPPING (see README.md "VI"):
//   Present the ngx-rendered frame through the host window swapchain and pace at the
//   display's vsync. CRUCIAL: VIWaitForRetrace is the game's 60 Hz HEARTBEAT — many
//   engine timers and the main loop tick off it, and the post-retrace callback runs
//   once per presented field. So the VI seam owns frame pacing + the present.
//
//   VIConfigure / VISetNextFrameBuffer / GXCopyDisp interplay: GX copies the EFB to
//   the "external framebuffer" then VI scans it out. Natively, GXCopyDisp (gx_seam)
//   produces the presented image and VISetNextFrameBuffer just designates which
//   ngx frame to show; VIFlush/VIWaitForRetrace present + block until the next vsync.
#pragma once
#include "platform_types.h"

namespace sb::platform::vi {

using RetraceCallback = void (*)(u32 retraceCount);

// ---- bring-up (vi/vifuncs.h) --------------------------------------------
void Init();                          // VIInit -> create the host window + swapchain
void Configure(const void* renderMode);  // VIConfigure(GXRenderModeObj*) -> set res/mode (TODO)

// ---- present + pacing ---------------------------------------------------
void SetNextFrameBuffer(void* fb);    // VISetNextFrameBuffer (TODO: designate ngx frame)
void Flush();                         // VIFlush (TODO: submit present)
void WaitForRetrace();                // VIWaitForRetrace -> present + block to next vsync;
                                      //   runs the post-retrace callback. THE HEARTBEAT. (TODO)
u32  GetRetraceCount();               // VIGetRetraceCount (TODO)
u32  GetNextField();                  // VIGetNextField (TODO)
void SetBlack(bool black);            // VISetBlack -> blank the present (TODO)

// ---- callbacks ----------------------------------------------------------
RetraceCallback SetPreRetraceCallback(RetraceCallback cb);   // TODO
RetraceCallback SetPostRetraceCallback(RetraceCallback cb);  // TODO

// ---- tv format (vi.h VI_TVMODE / VIGetTvFormat) -------------------------
u32  GetTvFormat();   // VIGetTvFormat -> report NTSC (or a configurable region) (TODO)

} // namespace sb::platform::vi
