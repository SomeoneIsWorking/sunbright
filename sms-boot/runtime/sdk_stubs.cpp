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
#include "../boot_stubs/stub_trace.h"

extern "C" {

// ---- (a) INTENTIONAL SEAM — OS threading (single-threaded PC engine — locks are no-ops) ----
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
// loop bodies that block on OSReceiveMessage). See decomp/sms callsites
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
// ---- (a) INTENTIONAL SEAM — OS misc (reset/sound-mode/interrupt hooks; no PC equivalent needed) ----
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

// ---- (a) INTENTIONAL SEAM — OS stopwatch (kept as zeros — no perf timing needed for bring-up) ----
void OSInitStopwatch(OSStopwatch*, char*) {}
void OSStartStopwatch(OSStopwatch*) {}
void OSStopStopwatch(OSStopwatch*) {}
OSTime OSCheckStopwatch(OSStopwatch*) { return 0; }
void OSResetStopwatch(OSStopwatch*) {}

// ---- (a) INTENTIONAL SEAM — OS font (returns nulls — SMS_NATIVE_PLATFORM=1 skips font rendering) ----
u16   OSGetFontEncode(void) { return 0; }
BOOL  OSInitFont(OSFontHeader*) { return TRUE; }
char* OSGetFontTexture(const char* s, void**, s32*, s32*, s32*) { return const_cast<char*>(s); }
char* OSGetFontWidth(const char* s, s32* w) { if (w) *w = 0; return const_cast<char*>(s); }

// ---- (a) INTENTIONAL SEAM — DC cache: no-op on x86 (coherent, no PPC cache-line management needed) ----
void DCInvalidateRange(void*, u32) {}
void DCFlushRange(void*, u32) {}
void DCStoreRange(void*, u32) {}
void DCFlushRangeNoSync(void*, u32) {}
void DCZeroRange(void*, u32) {}

// ---- (a) INTENTIONAL SEAM — ARAM base (ARAM DMA copies run inline at the enqueue site; base address is a GC-hardware detail no native caller reads) ----
u32 ARGetBaseAddress(void) { return 0; }

// ---- (a) INTENTIONAL SEAM — DSP mailboxes (no DSP coprocessor on PC; audio out is aurora::audio, see AI section) ----
u32 DSPCheckMailFromDSP(void) { return 0; }
u32 DSPReadMailFromDSP(void) { return 0; }

// ---- (a) INTENTIONAL SEAM — AI (audio interface — part of the documented audio-silence gap, CLAUDE.md "Named audio arc": aurora::audio owns output via sb_audio_frame, the JAS DSP mixer is not ported yet) ----
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

// ---- (a) INTENTIONAL SEAM — VI retrace counter (real frame pacing is sb_frame_present, runtime/frame_seam.cpp) ----
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
// GXClearPixMetric/GXReadPixMetric back TPollutionCount's on-screen goop-pixel counter
// (Map/PollutionCount.cpp) — a real gameplay mechanic (Mare/Ricco pollution meter), not
// yet reached by the title/file-select boot target. Aurora has no GX perf-counter
// pipeline (extern/aurora/lib/dolphin/gx/GXPerf.cpp marks both TODO), so this always
// reports zero pixels — a fabricated "no pollution visible" answer, not a real readback.
// SILENT LANDMINE: loud-once so the pollution meter doesn't quietly read as empty forever.
void GXClearPixMetric(void) {}
void GXReadPixMetric(u32* a, u32* b, u32* c, u32* d, u32* e, u32* f) {
    SB_STUB_HIT("GXReadPixMetric");
    if (a) *a = 0; if (b) *b = 0; if (c) *c = 0; if (d) *d = 0;
    if (e) *e = 0; if (f) *f = 0;
}
// GXEnableBreakPt/GXDisableBreakPt: INTENTIONAL SEAM, already documented at the
// callsite (decomp/sms/src/System/DrawSyncManager.cpp:124) — single-threaded/
// synchronous rendering has no async GP FIFO to breakpoint against.
void GXEnableBreakPt(void*) {}
void GXDisableBreakPt(void) {}
// GXWaitDrawDone: INTENTIONAL SEAM — synchronous rendering means the draw is always
// already done by the time anything could wait on it (no GP pipe latency to hide).
void GXWaitDrawDone(void) {}
GXDrawSyncCallback GXSetDrawSyncCallback(GXDrawSyncCallback cb) {
    GXDrawSyncCallback prev = s_drawSyncCallback;
    s_drawSyncCallback = cb;
    return prev;
}
// GXSetMisc: INTENTIONAL SEAM — GX_MT_XF_FLUSH/GX_MT_DL_SAVE_CONTEXT are GP-pipeline
// sync tokens for a real GC transform-unit FIFO; Aurora's fifo is unsynchronized by
// construction (single-threaded, no XF latency to flush against).
void GXSetMisc(GXMiscToken, u32) {}
// GXPokeAlphaRead: configures whether GXPeekARGB's alpha channel is read from the EFB.
// Paired with the GXPeekARGB landmine below — the mode setting itself writes no data,
// but see GXPeekARGB's comment for the landmine this feeds.
void GXPokeAlphaRead(GXAlphaReadMode) {}
// GXPeekARGB reads back a live EFB pixel — Player/MarioMain.cpp:301 samples the pixel
// under Mario's screen position (used for a gameplay color-based check). Aurora has no
// EFB readback path (GXCpu2Efb.h declares it, no .cpp body). Always-black is a
// fabricated answer, not a real sample. SILENT LANDMINE: loud-once.
void GXPeekARGB(u16, u16, u32* col) {
    SB_STUB_HIT("GXPeekARGB");
    // STOPGAP: return the 0x10 dst-alpha stamp (= "Mario NOT occluded").
    // MarioMain.cpp:302 tests (peek & 0xff000000) == 0x10000000; the previous 0
    // made the check fail every frame -> MARIO_FLAG_OCCLUDED permanently ON.
    // (Tested 2026-07-16: this was NOT the file-select black-patch cause — the
    // patches are unchanged either way — but not-occluded is the correct answer
    // whenever nothing covers Mario.) Proper fix: aurora EFB readback (GXCpu2Efb).
    if (col) *col = 0x10000000;
}
// GXSetCopyClamp/GXSetDispCopyFrame2Field: EFB->XFB copy clamp/interlace config.
// INTENTIONAL SEAM — Aurora manages its own framebuffer scaling internally and has
// no interlaced scan-out. (GXGetNumXfbLines/GXGetYScaleFactor used to be silent
// stubs HERE returning 0/1.0f — that zeroed every render mode's computed
// xfbHeight/viHeight in JDrama::CalcRenderModeXFBHeight. They are now faithful
// SDK ports in extern/aurora/lib/dolphin/gx/GXFrameBuffer.cpp.)
void GXSetCopyClamp(GXFBClamp) {}
void GXSetDispCopyFrame2Field(GXCopyMode) {}
// GXInitTexCacheRegion: GC TMEM texture-cache region bookkeeping (J3DSys.cpp / GXInit.c)
// — Aurora textures are uploaded through its own GPU texture path, not GC TMEM regions.
// INTENTIONAL SEAM.
void GXInitTexCacheRegion(GXTexRegion*, GXBool, u32, GXTexCacheSize, u32, GXTexCacheSize) {}

// ---- HAM movie-wipe controller (GC2D/hx_wiper.h): the 8 externally-referenced
// Hx_ symbols are a faithful native port in runtime/hx_wipe.cpp (recovered from
// the 2026-06-21/26 STAGE-A port and re-landed here; see that file's header
// comment for the full RE trail). Do NOT re-add no-op stubs for these here. ----

// ---- THP (movie playback) — MOVIE-SKIP SEAM -------------------------------
// The THP video/audio decode+render arc (JPEG-DCT frame decode, VI-retrace
// pump, GX texture upload — decomp/sms/src/THPPlayer/THPPlayer.c, EXCLUDED
// from the native build in CMakeLists.txt) is not ported. This is a minimal
// state machine that fakes an INSTANTLY-FINISHED movie so
// TMovieDirector::direct() (decomp/sms/src/System/MovieDirector.cpp)
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

// ---- J3DGD/JRN* (decomp's GD-buffer + Ninja-renderer BP-register writers) ----
// Real bodies now built natively from decomp/sms/src/JSystem/JRenderer.cpp
// (sms-boot/CMakeLists.txt no longer excludes it — see the comment there,
// 2026-07-10: these no-op stubs were silently dropping the ENTIRE J3D
// material BP-register stream, including JRNISetTevOrder's TREF write,
// which is why every TEV stage kept aurora's default texMapId=GX_TEXMAP_NULL
// and rendered geometry with color forced to 0 regardless of texture/RASC).
// Only JPABaseField's forward decls below are still needed by other stubs
// in this file.
struct _GXFogAdjTable;
struct JPADataBlock;
struct JPAParticle;
class JPABaseField { public: JPABaseField(); };

// (Removed: JPAField.cpp is now built as part of sms-native — see CMakeLists.txt.
// The stubs here previously provided empty ctors/methods that skipped critical
// JSUPtrLink init in JPABaseField::JPABaseField(), leaving fresh fields with
// uninitialized mPtrList/mPrev/mNext and crashing JSUPtrList::prepend on any
// SolidHeap reuse.)

// ---- JUTException / JUTDirectPrint (JUT*.cpp excluded — provide static storage
// + no-op methods so callers link) ----
//
// 💥 2026-07-21, ASan-verified startup memory corruption. This block previously
// declared its OWN minimal `class JUTException` / `class JUTDirectPrint` here
// rather than including the real headers. Those local classes had NO data
// members (size 1), so create()/start() handed back a 1-byte object — while
// every real caller (marerr.cpp) sees the REAL class and writes fields at their
// real offsets through it:
//
//   ERROR: AddressSanitizer: global-buffer-overflow
//   WRITE of size 8 at 0x... (in a global redzone)
//     #0 JUTException::setGamePad(JUTGamePad*)  JUTException.hpp:96   (mGamePad, ofs 0x68)
//     #1 MarErrInit()                           marerr.cpp:35
//     #2 TApplication::initialize()             Application.cpp:260
//
// A textbook ODR violation that silently smashed neighbouring globals on every
// boot. Including the REAL headers makes the type THE type, and the storage
// below is correctly sized + zero-initialised. No constructor runs (JKRThread's
// is not available natively) — acceptable because this is an exception/dev
// facility we never actually enter on PC; the methods below stay no-ops. If a
// caller ever needs real behaviour, port JUTException.cpp instead of widening
// this seam.
#include <JSystem/JUtility/JUTDirectPrint.hpp>
#include <JSystem/JUtility/JUTException.hpp>

JUTDirectPrint* JUTDirectPrint::sDirectPrint = nullptr;
JUTDirectPrint* JUTDirectPrint::start()
{
	alignas(JUTDirectPrint) static unsigned char storage[sizeof(JUTDirectPrint)];
	sDirectPrint = reinterpret_cast<JUTDirectPrint*>(storage);
	return sDirectPrint;
}
void JUTDirectPrint::changeFrameBuffer(void*, u16, u16) {}
void JUTDirectPrint::drawString(u16, u16, char*) {}
void JUTDirectPrint::erase(int, int, int, int) {}

JUTException* JUTException::sErrorManager = nullptr;
JUTException* JUTException::create(JUTDirectPrint*)
{
	alignas(JUTException) static unsigned char storage[sizeof(JUTException)];
	sErrorManager = reinterpret_cast<JUTException*>(storage);
	return sErrorManager;
}
void JUTException::createConsole(void*, u32) {}
void JUTException::appendMapFile(char*) {}
OSErrorHandler JUTException::setPreUserCallback(OSErrorHandler) { return nullptr; }
void JUTException::waitTime(s32) {}
// real signature returns bool (the old local stub declared it void — part of the
// same ODR divergence); no pad is ever read natively, so report "nothing pressed".
bool JUTException::readPad(u32* buttons, u32* trigger)
{
	if (buttons) *buttons = 0;
	if (trigger) *trigger = 0;
	return false;
}

// ---- sb_* capture no-ops still referenced by SMS_NATIVE_PLATFORM branches in
// decomp/sms (retired Path-B capture hooks; removing those callsites
// upstream retires these too) ----
// sb_boot_capture_set_phase / sb_boot_capture_phase moved to phase_track.cpp (real per-frame
// phase tracker restoring the [dbhead]/[mapxlu] diagnostics; the old stub here silently
// discarded the phase, and the sb_own_gxlist/sb_boot_capture_begin_scene/end_scene capture-lock
// gate they used to sit behind belonged to the deleted Path-B capture buffer — removed upstream.
//
// 2026-07-10 STUB AUDIT FINDING: these 5 definitions had drifted out of signature-sync
// with the `extern "C"` declarations at their actual callsites (CardLoad.cpp,
// MarDirectorDirect.cpp, J3DDrawBuffer.cpp, J3DModel.cpp) — e.g.
// sb_gx_get_color_alpha_update was declared `void(int*, int*)` at its callsite but
// DEFINED here as a zero-arg `int()`. extern "C" linkage resolves by NAME only, so this
// linked and ran with the caller's real args silently discarded (undefined behavior,
// exactly the JRenderer failure shape: a linkable stub whose signature lies about what
// it does). Fixed to match the real callsite signatures; kept as no-ops (all 5 sit
// behind retired-Path-B or env-gated SB_*_DBG diagnostic branches, never the render hot
// path) but the pointer-output ones now correctly do nothing INSTEAD of corrupting args.
extern "C" {
bool sb_boot_drive_scene() {
    // Path-B relic: forced TSmJ3DScn::perform(8) because Path-B's MActor-perform dispatch
    // never delivered bit 0x8 to the scene (see docs/DO_NOT_REVISIT_FLIP.md history).
    // Aurora (Path A) delivers scene draw through its own GX dispatch — the workaround
    // does not apply here (same reasoning as TSky::perform's Path-B-only MActor-bit
    // drop, debug_journal 2026-07-03). Caller (MarDirectorDirect.cpp) discards the
    // return value, so `true` vs the old `void` was never observable; kept as a no-op.
    return true;
}
void sb_boot_request_dump(int) {
    // Retired Path-B frame-dump-N-frames request (SB_SEL_DUMP/SB_SEL_DUMP_SETTLED
    // diagnostics in CardLoad.cpp); the capture buffer it drove was deleted with Path-B.
    // SB_DUMP_FRAME(_AFTER) is the current frame-capture diagnostic (aurora end_frame).
    SB_STUB_HIT("sb_boot_request_dump");
}
bool sb_camera_view_settled() {
    // SB_SEL_DUMP_SETTLED gate (CardLoad.cpp): "has the option-camera pan finished".
    // Always-settled is the conservative choice (never blocks the dump waiting on a
    // signal nothing produces) but is a fabricated answer, not a real query.
    SB_STUB_HIT("sb_camera_view_settled");
    return true;
}
void sb_gx_get_color_alpha_update(int* cu, int* au) {
    // SB_DBHEAD_PKT diagnostic (J3DDrawBuffer.cpp): live TEV cU/aU (color/alpha update
    // enable) at flush time. Caller pre-seeds *cu=*au=1 before calling — leaving the
    // out-params untouched (rather than not writing them at all) keeps that seeded
    // default rather than reading uninitialized memory, but it's still a fabricated
    // answer: the real cU/aU never gets read from the live TEV stage state.
    SB_STUB_HIT("sb_gx_get_color_alpha_update");
    (void)cu; (void)au;
}
void sb_gx_get_projection(int* projType, float* proj, float* vp) {
    // SB_J3D_DBG diagnostic (J3DModel.cpp): live projection type/matrix/viewport for
    // per-1000-frame telemetry. Caller passes uninitialized locals and prints them
    // regardless of what we do here — leave them as the caller's garbage rather than
    // fabricating plausible-looking numbers that would misrepresent real GX state.
    SB_STUB_HIT("sb_gx_get_projection");
    (void)projType; (void)proj; (void)vp;
}
}
