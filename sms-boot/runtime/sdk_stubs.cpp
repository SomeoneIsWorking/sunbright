// runtime/sdk_stubs.cpp — no-op impls for GC SDK surfaces Aurora doesn't
// provide and the decomp references. Under SMS_NATIVE_PLATFORM=1 the game's
// fast paths short-circuit most of these; the remaining call sites still need
// a linkable symbol. Each stub is intentionally minimal — real implementations
// replace them as the boot path exercises the feature.
// VI retrace / frame pacing lives in runtime/frame_seam.cpp, not here.

#include <dolphin/types.h>
#include <dolphin/os.h>
#include <dolphin/os/OSThread.h>
#include <dolphin/os/OSMutex.h>
#include <dolphin/os/OSMessage.h>
#include <dolphin/os/OSFont.h>
#include <dolphin/os/OSReset.h>
#include <dolphin/os/OSResetSW.h>
#include <dolphin/os/OSRtc.h>
#include <dolphin/os/OSMemory.h>
#include <dolphin/vi.h>
#include <dolphin/ai.h>
#include <dolphin/ar.h>
#include <dolphin/dsp.h>
#include <dolphin/os/OSCache.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXFifo.h>
#include <dolphin/gx/GXPerf.h>
#include <dolphin/gx/GXManage.h>
#include <dolphin/gx/GXCpu2Efb.h>
#include <dolphin/gx/GXFrameBuffer.h>
#include <dolphin/gx/GXTransform.h>
#include <dolphin/gx/GXTexture.h>
#include <dolphin/gx/GXGet.h>
#include <dolphin/gx/GXEnum.h>
#include <dolphin/thp.h>
#include <cstring>
#include <cstdlib>

extern "C" {

// ---- OS threading (single-threaded PC engine — locks are no-ops) ----
void OSInitMutex(OSMutex*) {}
void OSLockMutex(OSMutex*) {}
void OSUnlockMutex(OSMutex*) {}
BOOL OSTryLockMutex(OSMutex*) { return TRUE; }
void OSInitCond(OSCond*) {}
void OSSignalCond(OSCond*) {}
void OSWaitCond(OSCond*, OSMutex*) {}

// PC engine is single-threaded. OS threading primitives are pure no-ops; the
// game's OSCreateThread/OSResumeThread callsites decide synchronously whether
// to run the target function directly (setup threads) or skip it (worker
// loop bodies that block on OSReceiveMessage). See reference/sms callsites
// under SMS_NATIVE_PLATFORM.
void OSInitThreadQueue(OSThreadQueue*) {}
void OSSleepThread(OSThreadQueue*) {}
void OSWakeupThread(OSThreadQueue*) {}
OSThread* OSGetCurrentThread(void) { return nullptr; }
void OSCancelThread(OSThread*) {}
BOOL OSIsThreadTerminated(OSThread*) { return TRUE; }
void OSYieldThread(void) {}
BOOL OSCreateThread(OSThread*, void* (*)(void*), void*, void*, u32, s32, u16) { return TRUE; }
s32  OSResumeThread(OSThread*) { return 0; }
void OSExitThread(void*) {}
BOOL OSJoinThread(OSThread*, void**) { return TRUE; }
void OSDetachThread(OSThread*) {}
s32  OSGetThreadPriority(OSThread*) { return 0; }

// Real single-threaded message queues. Workers are gone (their loop bodies run
// inline at the enqueue sites — JKRDecomp::sendCommand, JKRAramPiece::orderAsync),
// but the COMPLETION protocol still flows through per-command OSMessageQueues
// (JKRDecomp::sync / JKRAramPiece::sync receive a done-message the inline body
// sent). The old no-op stubs silently discarded those sends, which is how whole
// subsystems (ARAM DMA, queued SZS decode) came to "succeed" without running.
// A blocking receive on an empty queue is a guaranteed single-thread deadlock:
// crash right there instead of spinning forever.
void OSInitMessageQueue(OSMessageQueue* mq, OSMessage* msgArray, s32 msgCount) {
    mq->msgArray = msgArray;
    mq->msgCount = msgCount;
    mq->firstIndex = 0;
    mq->usedCount = 0;
}
BOOL OSSendMessage(OSMessageQueue* mq, OSMessage msg, s32 flags) {
    if (mq->usedCount >= mq->msgCount) {
        if (flags & OS_MESSAGE_BLOCK)
            OSPanic(__FILE__, __LINE__,
                    "OSSendMessage: blocking send on full queue %p (count=%d) — "
                    "single-threaded deadlock", (void*)mq, (int)mq->msgCount);
        return FALSE;
    }
    mq->msgArray[(mq->firstIndex + mq->usedCount) % mq->msgCount] = msg;
    ++mq->usedCount;
    return TRUE;
}
BOOL OSReceiveMessage(OSMessageQueue* mq, OSMessage* msg, s32 flags) {
    if (mq->usedCount == 0) {
        if (flags & OS_MESSAGE_BLOCK)
            OSPanic(__FILE__, __LINE__,
                    "OSReceiveMessage: blocking receive on empty queue %p — "
                    "single-threaded deadlock (producer never ran?)", (void*)mq);
        return FALSE;
    }
    if (msg) *msg = mq->msgArray[mq->firstIndex];
    mq->firstIndex = (mq->firstIndex + 1) % mq->msgCount;
    --mq->usedCount;
    return TRUE;
}
BOOL OSJamMessage(OSMessageQueue* mq, OSMessage msg, s32 flags) {
    if (mq->usedCount >= mq->msgCount) {
        if (flags & OS_MESSAGE_BLOCK)
            OSPanic(__FILE__, __LINE__,
                    "OSJamMessage: blocking jam on full queue %p — "
                    "single-threaded deadlock", (void*)mq);
        return FALSE;
    }
    mq->firstIndex = (mq->firstIndex + mq->msgCount - 1) % mq->msgCount;
    mq->msgArray[mq->firstIndex] = msg;
    ++mq->usedCount;
    return TRUE;
}

// ---- OSReport (Aurora declares weak but has no impl) ----
#include <cstdarg>
void OSReport(const char* fmt, ...) {
    std::va_list ap; va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}
void OSPanic(const char* file, int line, const char* fmt, ...) {
    std::fprintf(stderr, "OSPanic %s:%d: ", file, line);
    std::va_list ap; va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    abort();
}
// ---- OS misc ----
BOOL OSEnableInterrupts(void) { return TRUE; }
BOOL OSDisableInterrupts(void) { return TRUE; }
BOOL OSRestoreInterrupts(BOOL) { return TRUE; }
void OSProtectRange(u32, void*, u32, u32) {}
void OSResetSystem(int, u32, BOOL) {}
BOOL OSGetResetSwitchState(void) { return FALSE; }
u32  OSGetSoundMode(void) { return 1; }
void OSSetSoundMode(u32) {}
u32  OSGetProgressiveMode(void) { return 0; }
void OSSetProgressiveMode(u32) {}

// ---- OS stopwatch (kept as zeros — no perf timing needed for bring-up) ----
void OSInitStopwatch(OSStopwatch*, char*) {}
void OSStartStopwatch(OSStopwatch*) {}
void OSStopStopwatch(OSStopwatch*) {}
OSTime OSCheckStopwatch(OSStopwatch*) { return 0; }
void OSResetStopwatch(OSStopwatch*) {}

// ---- OS font (returns nulls — SMS_NATIVE_PLATFORM=1 skips font rendering) ----
u16   OSGetFontEncode(void) { return 0; }
BOOL  OSInitFont(OSFontHeader*) { return TRUE; }
char* OSGetFontTexture(const char* s, void**, s32*, s32*, s32*) { return const_cast<char*>(s); }
char* OSGetFontWidth(const char* s, s32* w) { if (w) *w = 0; return const_cast<char*>(s); }

// ---- DC cache — no-op on x86 (coherent) ----
void DCInvalidateRange(void*, u32) {}
void DCFlushRange(void*, u32) {}
void DCStoreRange(void*, u32) {}
void DCFlushRangeNoSync(void*, u32) {}
void DCZeroRange(void*, u32) {}

// ---- ARAM base ----
u32 ARGetBaseAddress(void) { return 0; }

// ---- DSP mailboxes ----
u32 DSPCheckMailFromDSP(void) { return 0; }
u32 DSPReadMailFromDSP(void) { return 0; }

// ---- AI (audio interface — Aurora doesn't drive AI; native audio owns output) ----
void AIInit(u8*) {}
void AIInitDMA(u32, u32) {}
void AIStartDMA(void) {}
AIDCallback AIRegisterDMACallback(AIDCallback) { return nullptr; }
void AIResetStreamSampleCount(void) {}
void AISetStreamPlayState(u32) {}
void AISetDSPSampleRate(u32) {}
void AISetStreamSampleRate(u32) {}
void AISetStreamVolLeft(u8) {}
void AISetStreamVolRight(u8) {}

// ---- VI retrace counter ----
// VIWaitForRetrace is a PURE COUNTER, not the frame boundary: the game also
// calls it from load-polling spin loops and TV-mode settle loops (all of
// which complete instantly now that I/O is synchronous). The real per-frame
// present/pump/pace seam is sb_frame_present(), called from JDrama::
// TVideo::waitForRetrace (runtime/frame_seam.cpp).
static u32 s_retraceCount = 0;
void VIWaitForRetrace(void) { ++s_retraceCount; }
u32  VIGetRetraceCount(void) { return s_retraceCount; }
u32  VIGetNextField(void) { return 0; }
u32  VIGetDTVStatus(void) { return 0; }
void VISetBlack(BOOL) {}
void VISetNextFrameBuffer(void*) {}

// ---- GX gaps (Aurora provides most; these are the last pieces) ----
// Draw-sync tokens: on GC the GP raises an interrupt when the token drains
// through the pipe; the callback (TDrawSyncManager::callbackDrawSync,
// Application.cpp:281) retires pending draw-sync waiters. Single-threaded
// PC has no pipe latency — the "draw" is complete by the time the token is
// set, so dispatch the callback inline.
static GXDrawSyncCallback s_drawSyncCallback = nullptr;
void GXSetDrawSync(u16 token) {
    if (s_drawSyncCallback) s_drawSyncCallback(token);
}
void GXClearPixMetric(void) {}
void GXReadPixMetric(u32* a, u32* b, u32* c, u32* d, u32* e, u32* f) {
    if (a) *a = 0; if (b) *b = 0; if (c) *c = 0; if (d) *d = 0;
    if (e) *e = 0; if (f) *f = 0;
}
void GXEnableBreakPt(void*) {}
void GXDisableBreakPt(void) {}
void GXWaitDrawDone(void) {}
GXDrawSyncCallback GXSetDrawSyncCallback(GXDrawSyncCallback cb) {
    GXDrawSyncCallback prev = s_drawSyncCallback;
    s_drawSyncCallback = cb;
    return prev;
}
void GXSetMisc(GXMiscToken, u32) {}
void GXPokeAlphaRead(GXAlphaReadMode) {}
void GXPeekARGB(u16, u16, u32* col) { if (col) *col = 0; }
void GXSetCopyClamp(GXFBClamp) {}
void GXSetDispCopyFrame2Field(GXCopyMode) {}
u16  GXGetNumXfbLines(u16, f32) { return 0; }
f32  GXGetYScaleFactor(u16, u16) { return 1.0f; }
void GXInitTexCacheRegion(GXTexRegion*, GXBool, u32, GXTexCacheSize, u32, GXTexCacheSize) {}

// ---- HAM movie-wipe controller (retired — SMS_NATIVE_PLATFORM=1 uses direct
// scene transitions; no-op stubs let the callers link) ----
void Hx_ResetWipe(u32, u32) {}
void Hx_StartWipe(int, int) {}
u32  Hx_UpdateWipe(f32) { return 0; }
int  Hx_GetWipeType(int) { return 0; }
int  Hx_MovieStartSyncEx() { return 0; }
void Hx_ProvideResource(void*, int) {}
void Hx_ProvideResourceEx(void*) {}
void Hx_RemoveResource() {}

// ---- THP (movie playback) — MOVIE-SKIP SEAM -------------------------------
// The THP video/audio decode+render arc (JPEG-DCT frame decode, VI-retrace
// pump, GX texture upload — reference/sms/src/THPPlayer/THPPlayer.c, EXCLUDED
// from the native build in CMakeLists.txt) is not ported. This is a minimal
// state machine that fakes an INSTANTLY-FINISHED movie so
// TMovieDirector::direct() (reference/sms/src/System/MovieDirector.cpp)
// takes its own, already-ported, real end-of-playback branch instead of
// spinning forever in STATE_PLAYING waiting for a frame count that will
// never advance. DELETE this whole block (and the sThp* statics) once the
// real THP decode/render arc lands.
//
// Real ActivePlayer.state values, reverse-engineered from THPPlayer.c (no
// named enum there — just raw assignments to the u8 `state` field):
//   0 = idle / not playing        (THPPlayerOpen L199-203, THPPlayerStop L450-451)
//   1 = prepared, ready to play   (THPPlayerPrepare success, L419)
//   2 = playing                   (THPPlayerPlay success, L438)
//   3 = FINISHED — last frame decoded and all buffers drained; the
//       ORDINARY, non-error end of a movie (PlayControl(), L551-565)
//   4 = paused                    (THPPlayerPause, L476-477)
//   5 = DVD/video decode ERROR    (PlayControl(), L492-495 — NOT reached by
//       a normal end of playback, only by a corrupt/short DVD read)
//
// TMovieDirector::direct()'s STATE_PLAYING case (MovieDirector.cpp:335-351)
// polls THPPlayerGetState() every frame:
//   == 5 -> nextState = STATE_FADE_OUT directly (the player's ERROR path)
//   == 3 -> desiredAppState = decideNextMode(&nextState) (the player's
//           NORMAL end-of-playback path — already correct for every movie,
//           incl. the title idle-attract autodemoA: movie==12 sets
//           mNextArea=(15,0,0) and returns APP_STATE 5, back to title
//           stage 15).
// A movie that finishes normally reports state 3, not 5 (5 is the DVD/codec
// error branch) — so this stub fakes 3, taking the SAME code path a real
// completed movie takes rather than borrowing the unrelated error branch.
static bool sThpOpen  = false;
static u8   sThpState = 0; // idle

BOOL THPPlayerInit() { sThpOpen = false; sThpState = 0; return TRUE; }
void THPPlayerQuit() { sThpOpen = false; sThpState = 0; }

BOOL THPPlayerOpen(const char*, BOOL) {
    // Real THPPlayerOpen parses the .thp header off DVD (THPPlayer.c:120-211).
    // We never decode a real movie, so any filename opens successfully.
    sThpOpen  = true;
    sThpState = 0;
    return TRUE;
}

BOOL THPPlayerClose() {
    if (sThpOpen && sThpState == 0) { sThpOpen = false; return TRUE; }
    return FALSE;
}

u32  THPPlayerCalcNeedMemory() { return 0; }
BOOL THPPlayerSetBuffer(u8*) { return sThpOpen; }

BOOL THPPlayerPrepare(s32, s32, s32, s32) {
    if (sThpOpen && sThpState == 0) { sThpState = 1; return TRUE; }
    return FALSE;
}

BOOL THPPlayerPlay() {
    if (sThpOpen && (sThpState == 1 || sThpState == 4)) {
        // Real THPPlayerPlay() moves state 1|4 -> 2 (playing) and lets the
        // VI-retrace PlayControl() callback tick frames until it naturally
        // reaches state 3. We decode/render nothing, so skip straight to
        // 3 — a zero-duration movie that still drives the engine's real
        // end-of-playback branch above instead of a fake/bypassed one.
        sThpState = 3;
        return TRUE;
    }
    return FALSE;
}

BOOL THPPlayerPause() {
    if (sThpOpen && sThpState == 2) { sThpState = 4; return TRUE; }
    return FALSE;
}

BOOL THPPlayerStop() {
    if (sThpOpen) sThpState = 0;
    return TRUE;
}

u32  THPPlayerGetState() { return sThpState; }

u32  THPPlayerDrawCurrentFrame(void*, void*, u32, u32, u32) { return 0; } // no frame ever decoded
BOOL THPPlayerDrawDone() { return TRUE; }

BOOL THPPlayerGetAudioInfo(THPAudioInfo*) { return FALSE; } // no audio track decoded

BOOL THPPlayerGetVideoInfo(THPVideoInfo* info) {
    // MovieDirector::rsetup() (MovieDirector.cpp:177) reads this unconditionally
    // (the BOOL result is discarded) to size TTHPRender::setParams; leaving
    // *info uninitialized reads garbage stack memory into the render rect.
    // 640x480 matches the usual NTSC game-render frame size; videoType=0 =
    // progressive (no interlace field bits set) — sane placeholder dims for
    // a movie that never actually decodes a frame.
    if (info) { info->xSize = 640; info->ySize = 480; info->videoType = 0; }
    return sThpOpen;
}

void THPPlayerSetVolume(s32, s32) {}

} // extern "C"

// ---- J3DGD (decomp's GD-buffer helpers into GX-style calls — usually inlined
// into display-lists; PC engine renders through Aurora directly, so these are
// no-ops for now) ----
struct _GXFogAdjTable;
struct JPADataBlock;
struct JPAParticle;
class JPABaseField { public: JPABaseField(); };

void J3DGDLoadTexMtxImm(float (*)[4], unsigned int, GXTexMtxType) {}
void J3DGDLoadTlut(void*, unsigned int, GXTlutSize) {}
void J3DGDSetChanAmbColor(GXChannelID, GXColor) {}
void J3DGDSetChanCtrl(GXChannelID, unsigned char, GXColorSrc, GXColorSrc, unsigned int, GXDiffuseFn, GXAttnFn) {}
void J3DGDSetChanMatColor(GXChannelID, GXColor) {}
void J3DGDSetFog(GXFogType, float, float, float, float, GXColor) {}
void J3DGDSetTevKColor(GXTevKColorID, GXColor) {}
void J3DGDSetTexCoordGen(GXTexCoordID, GXTexGenType, GXTexGenSrc, unsigned char, unsigned int) {}
void J3DGDSetTexCoordScale2(GXTexCoordID, unsigned short, unsigned char, unsigned char, unsigned short, unsigned char, unsigned char) {}
void J3DGDSetTexImgAttr(GXTexMapID, unsigned short, unsigned short, GXTexFmt) {}
void J3DGDSetTexImgPtr(GXTexMapID, void*) {}
void J3DGDSetTexTlut(GXTexMapID, unsigned int, GXTlutFmt) {}

// ---- JRN* (JSystem Ninja renderer indirect-TEV/fog helpers) ----
void JRNISetFogRangeAdj(bool, unsigned short, _GXFogAdjTable*) {}
void JRNISetTevColorS10(GXTevRegID, GXColorS10) {}
void JRNISetTevOrder(GXTevStageID, GXTexCoordID, GXTexMapID, GXChannelID, GXTexCoordID, GXTexMapID, GXChannelID) {}
void JRNLoadCurrentMtx(unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int) {}
void JRNLoadTexCached(GXTexMapID, unsigned int, GXTexCacheSize, unsigned int, GXTexCacheSize) {}
void JRNSetIndTexCoordScale(GXIndTexStageID, GXIndTexScale, GXIndTexScale, GXIndTexScale, GXIndTexScale) {}
void JRNSetIndTexMtx(GXIndTexMtxID, float (*)[3], signed char) {}
void JRNSetIndTexOrder(unsigned int, GXTexCoordID, GXTexMapID, GXTexCoordID, GXTexMapID, GXTexCoordID, GXTexMapID, GXTexCoordID, GXTexMapID) {}
void JRNSetTevIndirect(GXTevStageID, GXIndTexStageID, GXIndTexFormat, GXIndTexBiasSel, GXIndTexMtxID, GXIndTexWrap, GXIndTexWrap, bool, bool, GXIndTexAlphaSel) {}

// (Removed: JPAField.cpp is now built as part of sms-native — see CMakeLists.txt.
// The stubs here previously provided empty ctors/methods that skipped critical
// JSUPtrLink init in JPABaseField::JPABaseField(), leaving fresh fields with
// uninitialized mPtrList/mPrev/mNext and crashing JSUPtrList::prepend on any
// SolidHeap reuse.)

// ---- JUTException / JUTDirectPrint (JUT*.cpp excluded — provide static storage
// + no-op methods so callers link) ----
class JUTDirectPrint {
public:
    static JUTDirectPrint* sDirectPrint;
    static JUTDirectPrint* start();
    void changeFrameBuffer(void*, unsigned short, unsigned short);
    void drawString(unsigned short, unsigned short, char*);
    void erase(int, int, int, int);
};
JUTDirectPrint* JUTDirectPrint::sDirectPrint = nullptr;
JUTDirectPrint* JUTDirectPrint::start() { static JUTDirectPrint p; sDirectPrint = &p; return &p; }
void JUTDirectPrint::changeFrameBuffer(void*, unsigned short, unsigned short) {}
void JUTDirectPrint::drawString(unsigned short, unsigned short, char*) {}
void JUTDirectPrint::erase(int, int, int, int) {}

class JUTException {
public:
    static JUTException* sErrorManager;
    static JUTException* create(JUTDirectPrint*);
    static void createConsole(void*, unsigned int);
    static void appendMapFile(char*);
    static void setPreUserCallback(void (*)(unsigned short, OSContext*, ...));
    static void waitTime(int);
    void readPad(unsigned int*, unsigned int*);
};
JUTException* JUTException::sErrorManager = nullptr;
JUTException* JUTException::create(JUTDirectPrint*) { static JUTException e; sErrorManager = &e; return &e; }
void JUTException::createConsole(void*, unsigned int) {}
void JUTException::appendMapFile(char*) {}
void JUTException::setPreUserCallback(void (*)(unsigned short, OSContext*, ...)) {}
void JUTException::waitTime(int) {}
void JUTException::readPad(unsigned int* a, unsigned int* b) { if (a) *a = 0; if (b) *b = 0; }

// ---- sb_* capture no-ops still referenced by SMS_NATIVE_PLATFORM branches in
// reference/sms (retired Path-B capture hooks; removing those callsites
// upstream retires these too) ----
extern "C" {
void sb_boot_capture_begin_scene(int) {}
void sb_boot_capture_end_scene() {}
void sb_boot_capture_set_phase(int) {}
void sb_boot_drive_scene() {}
void sb_boot_request_dump(const char*) {}
bool sb_camera_view_settled() { return true; }
int  sb_gx_get_color_alpha_update() { return 0; }
void sb_gx_get_projection(float*) {}
void sb_own_gxlist() {}
}
