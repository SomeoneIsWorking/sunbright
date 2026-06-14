// ===========================================================================
// port/pal/io/vi_stub.cpp — PAL stubs for the GameCube VI (video interface)
// configuration / retrace / framebuffer API.
//
// Companion to pal/io/vi_dvd_stub.cpp (which defines VIGetRetraceCount /
// VIGetNextField / DVDConvertPathToEntrynum). This file resolves the remaining
// VI* symbols the SMS engine's JDrama video path (JDRVideo / JDRResolution /
// Application) references but that have no native hardware yet:
//   VIConfigure, VIFlush, VIGetTvFormat, VISetBlack, VISetNextFrameBuffer,
//   VIWaitForRetrace.
//
// SIGNATURES: from <dolphin/vi/vifuncs.h> (extern "C"), so they resolve.
//
// BEHAVIOR (bring-up placeholders — FLAG FOR PM):
//   - VIConfigure / VIFlush / VISetBlack / VISetNextFrameBuffer: no-ops (no real
//     VI register file; framebuffer presentation belongs to the native renderer).
//   - VIGetTvFormat: returns 0 (== VI_NTSC). SMS is region-NTSC for our ROM; the
//     engine branches on this for field timing. Replace if a PAL/region option
//     is wired up.
//   - VIWaitForRetrace: blocks one NTSC field period against the host clock so
//     callers that pace on it (the main render/update loop) advance at ~59.94 Hz
//     instead of spinning. This mirrors the host-clock field rate used by
//     vi_dvd_stub.cpp's VIGetRetraceCount, keeping the two consistent.
//     FLAG FOR PM: this paces on WALL TIME, not on actual presented fields — it
//     is a bring-up pacer, not a real VBlank sync. The native present loop must
//     replace it.
// ===========================================================================

#include <dolphin/vi/vifuncs.h>

#include <chrono>
#include <thread>

extern "C" {

void VIConfigure(GXRenderModeObj* /*rm*/) {}
void VIFlush(void) {}

u32 VIGetTvFormat(void) {
    // 0 == VI_NTSC in the SDK's VITVMode enum. Our ROM (GMSE01) is NTSC-U.
    return 0;
}

void VISetBlack(BOOL /*black*/) {}
void VISetNextFrameBuffer(void* /*fb*/) {}

void VIWaitForRetrace(void) {
    // Sleep one NTSC field (~16.683 ms) so retrace-paced loops advance over wall
    // time instead of busy-spinning. Bring-up pacer; see header comment.
    std::this_thread::sleep_for(std::chrono::nanoseconds(16683333));
}

}  // extern "C"
