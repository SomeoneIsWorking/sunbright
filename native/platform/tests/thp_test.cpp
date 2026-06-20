// thp_test.cpp — unit tests for the native THP player seam (thp_impl.cpp).
//
// Tests the state machine faithfully against the decomp semantics
// (reference/sms/src/THPPlayer/THPPlayer.c). All expected values are
// hand-derived from the decomp source — not from running the seam and
// recording output.
//
// State values (from the decomp):
//   0 = Stop/Closed   1 = Prepared   2 = Playing   3 = Done
//   4 = Paused        5 = Error     -1 = inactive (GetState not called yet)

#include <THPPlayer/THPPlayer.h>
#include <cstdio>
#include <cstring>
#include <cmath>

// ---------------------------------------------------------------------------
// Tiny assert harness (mirrors gx_test.cpp style)
// ---------------------------------------------------------------------------
static int g_fail = 0, g_checks = 0;

static void chki(long got, long want, const char* what) {
    ++g_checks;
    if (got != want) {
        ++g_fail;
        std::printf("  FAIL: %s  got=%ld  want=%ld\n", what, got, want);
    }
}

static void chkb(bool got, bool want, const char* what) {
    chki((long)got, (long)want, what);
}

static void chkf(float got, float want, const char* what, float eps = 1e-4f) {
    ++g_checks;
    if (std::fabs(got - want) > eps) {
        ++g_fail;
        std::printf("  FAIL: %s  got=%.6f  want=%.6f\n", what, got, want);
    }
}

// ---------------------------------------------------------------------------
// Helper: reset global state between tests by calling Quit + Init.
// ---------------------------------------------------------------------------
static void reset_player() {
    THPPlayerQuit();
    THPPlayerInit();
}

// ---------------------------------------------------------------------------
// test_init
// After THPPlayerInit():
//   - Returns TRUE
//   - ActivePlayer is zeroed (state=0, open=FALSE)
//   - GetState returns 0
// ---------------------------------------------------------------------------
static void test_init() {
    std::printf("[test_init]\n");

    BOOL ok = THPPlayerInit();
    chkb(ok == TRUE, true, "Init returns TRUE");
    // state is zero after memset
    chki(THPPlayerGetState(), 0, "state after Init = 0 (Stop)");
    chkb(ActivePlayer.open == FALSE, true, "open=FALSE after Init");
    chkb(ActivePlayer.state == 0,    true, "ActivePlayer.state == 0 after Init");
}

// ---------------------------------------------------------------------------
// test_open_close
// After Init → Open(_, FALSE):
//   - Returns TRUE; open=TRUE; state=0
//   - GetState still 0 (Open does not advance state)
//   - Close: returns TRUE, open=FALSE
//   - Double-open fails (open guard)
// ---------------------------------------------------------------------------
static void test_open_close() {
    std::printf("[test_open_close]\n");
    reset_player();

    BOOL opened = THPPlayerOpen("movie.thp", FALSE);
    chkb(opened == TRUE, true, "Open returns TRUE");
    chkb(ActivePlayer.open == TRUE, true, "open=TRUE after Open");
    chki(THPPlayerGetState(), 0, "state after Open = 0 (not yet Prepared)");
    chkf(ActivePlayer.curVolume, 127.0f, "curVolume initialised to 127");

    // Double-open must fail.
    BOOL opened2 = THPPlayerOpen("other.thp", FALSE);
    chkb(opened2 == FALSE, true, "second Open fails while already open");

    // Close succeeds: state==0 and open.
    BOOL closed = THPPlayerClose();
    chkb(closed == TRUE, true, "Close returns TRUE");
    chkb(ActivePlayer.open == FALSE, true, "open=FALSE after Close");

    // Close again must fail (not open).
    BOOL closed2 = THPPlayerClose();
    chkb(closed2 == FALSE, true, "second Close fails while not open");
}

// ---------------------------------------------------------------------------
// test_prepare
// Init → Open → Prepare(0, 0, 0):
//   - Returns TRUE; state becomes 1 (Prepared).
//   - GetState returns 1.
//   - Play after Prepare → state 2.
// Prepare when NOT open must fail.
// Prepare when state != 0 must fail.
// ---------------------------------------------------------------------------
static void test_prepare() {
    std::printf("[test_prepare]\n");
    reset_player();

    // Prepare without open → fail.
    BOOL r = THPPlayerPrepare(0, 0, 0);
    chkb(r == FALSE, true, "Prepare without Open fails");

    THPPlayerOpen("movie.thp", FALSE);

    BOOL prep = THPPlayerPrepare(0, 0, 0);
    chkb(prep == TRUE, true, "Prepare(0,0,0) returns TRUE");
    chki(THPPlayerGetState(), 1, "state after Prepare = 1 (Prepared)");
    chkb(ActivePlayer.dispTextureSet == nullptr, true, "dispTextureSet=null after Prepare");
    chkb(ActivePlayer.curVideoNumber == 0, true, "curVideoNumber=0 after Prepare");

    // Second Prepare must fail (state != 0).
    BOOL prep2 = THPPlayerPrepare(0, 0, 0);
    chkb(prep2 == FALSE, true, "second Prepare fails (state=1 not 0)");
}

// ---------------------------------------------------------------------------
// test_prepare_with_seek
// Prepare with frame>0 and no offset table (header.offsetDataOffsets==0) → FALSE.
// frame >= numFrames → FALSE.
// ---------------------------------------------------------------------------
static void test_prepare_seek_guard() {
    std::printf("[test_prepare_seek_guard]\n");
    reset_player();
    THPPlayerOpen("movie.thp", FALSE);

    // No offset data offsets in header → seek to frame 5 must fail.
    ActivePlayer.header.offsetDataOffsets = 0;
    ActivePlayer.header.numFrames = 100;
    BOOL r = THPPlayerPrepare(5, 0, 0);
    chkb(r == FALSE, true, "Prepare(frame>0) with offsetDataOffsets==0 fails");

    // Reset state to allow another Prepare.
    ActivePlayer.header.offsetDataOffsets = 0x1000;
    // frame >= numFrames → fail.
    r = THPPlayerPrepare(100, 0, 0);
    chkb(r == FALSE, true, "Prepare(frame>=numFrames) fails");
}

// ---------------------------------------------------------------------------
// test_play_stop_pause
// Full state machine: Stop(0) → Prepared(1) → Playing(2) → Paused(4) → Playing(2) → Stop(0)
// ---------------------------------------------------------------------------
static void test_play_stop_pause() {
    std::printf("[test_play_stop_pause]\n");
    reset_player();
    THPPlayerOpen("movie.thp", FALSE);
    THPPlayerPrepare(0, 0, 0);

    chki(THPPlayerGetState(), 1, "state before Play = 1");

    // Play from Prepared.
    BOOL played = THPPlayerPlay();
    chkb(played == TRUE, true, "Play() returns TRUE from Prepared");
    chki(THPPlayerGetState(), 2, "state after Play = 2 (Playing)");
    chki(ActivePlayer.prevCount, 0, "prevCount reset to 0 by Play");
    chki(ActivePlayer.curCount,  0, "curCount reset to 0 by Play");
    chki(ActivePlayer.retaceCount, -1, "retaceCount = -1 by Play");

    // Pause from Playing.
    BOOL paused = THPPlayerPause();
    chkb(paused == TRUE, true, "Pause() returns TRUE from Playing");
    chki(THPPlayerGetState(), 4, "state after Pause = 4 (Paused)");
    chki(ActivePlayer.internalState, 4, "internalState=4 after Pause");

    // Pause again must fail (not in Playing state).
    BOOL paused2 = THPPlayerPause();
    chkb(paused2 == FALSE, true, "second Pause fails (not Playing)");

    // Play from Paused (state==4 is also allowed by the decomp guard).
    BOOL played2 = THPPlayerPlay();
    chkb(played2 == TRUE, true, "Play() from Paused returns TRUE");
    chki(THPPlayerGetState(), 2, "state after Play from Paused = 2");

    // Stop.
    THPPlayerStop();
    chki(THPPlayerGetState(), 0, "state after Stop = 0");
    chki(ActivePlayer.internalState, 0, "internalState=0 after Stop");
    chki(ActivePlayer.dvdError,   0, "dvdError cleared by Stop");
    chki(ActivePlayer.videoError, 0, "videoError cleared by Stop");
    chkf(ActivePlayer.curVolume, ActivePlayer.targetVolume,
         "curVolume=targetVolume after Stop");
    chki(ActivePlayer.rampCount, 0, "rampCount=0 after Stop");
}

// ---------------------------------------------------------------------------
// test_play_guard
// Play must fail when state is not 1 or 4.
// ---------------------------------------------------------------------------
static void test_play_guard() {
    std::printf("[test_play_guard]\n");
    reset_player();

    // Not open.
    BOOL r = THPPlayerPlay();
    chkb(r == FALSE, true, "Play without open fails");

    THPPlayerOpen("movie.thp", FALSE);
    // state==0, not Prepared → Play must fail.
    r = THPPlayerPlay();
    chkb(r == FALSE, true, "Play from state=0 (not Prepared) fails");
}

// ---------------------------------------------------------------------------
// test_stop_guard
// THPPlayerStop is a void; just verify it doesn't crash when state==0 or not open.
// ---------------------------------------------------------------------------
static void test_stop_guard() {
    std::printf("[test_stop_guard]\n");
    reset_player();

    // stop when not open should be a no-op (doesn't crash).
    THPPlayerStop();  // must not crash
    chki(THPPlayerGetState(), 0, "GetState still 0 after Stop-when-not-open");

    THPPlayerOpen("movie.thp", FALSE);
    // state==0 — Stop guard: does nothing (state stays 0).
    THPPlayerStop();
    chki(THPPlayerGetState(), 0, "state=0 unchanged by Stop when already stopped");
}

// ---------------------------------------------------------------------------
// test_get_video_audio_info
// Set videoInfo/audioInfo on the struct manually; GetVideoInfo/GetAudioInfo copy them out.
// Guard: must return FALSE when not open.
// ---------------------------------------------------------------------------
static void test_get_video_audio_info() {
    std::printf("[test_get_video_audio_info]\n");
    reset_player();

    THPVideoInfo vinfo{};
    THPAudioInfo ainfo{};

    // Not open → both return FALSE.
    chkb(THPPlayerGetVideoInfo(&vinfo) == FALSE, true, "GetVideoInfo FALSE when closed");
    chkb(THPPlayerGetAudioInfo(&ainfo) == FALSE, true, "GetAudioInfo FALSE when closed");

    THPPlayerOpen("movie.thp", FALSE);

    // Write known values into the player struct fields directly (simulates what Open
    // would populate from the THP header on GC after DVD reads).
    ActivePlayer.videoInfo.xSize     = 640;
    ActivePlayer.videoInfo.ySize     = 480;
    ActivePlayer.videoInfo.videoType = 2;  // even-field progressive

    ActivePlayer.audioInfo.sndChannels  = 2;
    ActivePlayer.audioInfo.sndFrequency = 44100;
    ActivePlayer.audioInfo.sndNumSamples = 8820;
    ActivePlayer.audioInfo.sndNumTracks  = 1;

    BOOL vok = THPPlayerGetVideoInfo(&vinfo);
    chkb(vok == TRUE, true, "GetVideoInfo TRUE when open");
    chki(vinfo.xSize,     640, "videoInfo.xSize");
    chki(vinfo.ySize,     480, "videoInfo.ySize");
    chki(vinfo.videoType, 2,   "videoInfo.videoType");

    BOOL aok = THPPlayerGetAudioInfo(&ainfo);
    chkb(aok == TRUE, true, "GetAudioInfo TRUE when open");
    chki(ainfo.sndChannels,   2,     "audioInfo.sndChannels");
    chki(ainfo.sndFrequency,  44100, "audioInfo.sndFrequency");
    chki(ainfo.sndNumSamples, 8820,  "audioInfo.sndNumSamples");
    chki(ainfo.sndNumTracks,  1,     "audioInfo.sndNumTracks");
}

// ---------------------------------------------------------------------------
// test_calc_need_memory
// CalcNeedMemory is a pure arithmetic function; test it with known header values.
//
// The decomp formula (ALIGN_NEXT(x,32)):
//   onMemory=FALSE: read_slice = align32(bufsize) * 10
//   Y texture slice  = align32(xSize*ySize)        * 3
//   U texture slice  = align32(xSize*ySize/4)      * 3
//   V texture slice  = align32(xSize*ySize/4)      * 3
//   audio slice      = align32(audioMaxSamples*4)  * 3   (if audioExist)
//   total            = sum + 0x1000
//
// Hand-computed with xSize=64, ySize=64, bufsize=128, audioMaxSamples=256:
//   read   = align32(128)*10   = 128*10      = 1280
//   yslice = align32(64*64)*3  = 4096*3     = 12288
//   uvslice= align32(64*64/4)*3= 1024*3     = 3072   (x2 for U and V)
//   audio  = align32(256*4)*3  = 1024*3     = 3072   (if audioExist)
//   total (no audio) = 1280+12288+3072+3072+0x1000 = 23808
//   total (audio)    = 23808+3072 = 26880
// ---------------------------------------------------------------------------
static void test_calc_need_memory() {
    std::printf("[test_calc_need_memory]\n");
    reset_player();

    // Not open → 0.
    chki((long)THPPlayerCalcNeedMemory(), 0L, "CalcNeedMemory=0 when not open");

    THPPlayerOpen("movie.thp", FALSE);
    ActivePlayer.videoInfo.xSize = 64;
    ActivePlayer.videoInfo.ySize = 64;
    ActivePlayer.header.bufsize          = 128;
    ActivePlayer.header.movieDataSize    = 0;     // irrelevant (onMemory=FALSE)
    ActivePlayer.header.audioMaxSamples  = 256;
    ActivePlayer.audioExist              = 0;

    // No audio.
    u32 sz_no_audio = THPPlayerCalcNeedMemory();
    // read: align32(128)*10 = 1280
    // Y:    align32(64*64)*3 = 4096*3 = 12288
    // U:    align32(1024)*3 = 1024*3 = 3072
    // V:    3072
    // page: 0x1000 = 4096
    // total = 1280+12288+3072+3072+4096 = 23808
    chki((long)sz_no_audio, 23808L, "CalcNeedMemory no-audio (64x64 bufsize=128)");

    // With audio.
    ActivePlayer.audioExist = 1;
    u32 sz_audio = THPPlayerCalcNeedMemory();
    // audio: align32(256*4)*3 = align32(1024)*3 = 1024*3 = 3072
    // total = 23808+3072 = 26880
    chki((long)sz_audio, 26880L, "CalcNeedMemory with-audio (audioMaxSamples=256)");

    // onMemory=TRUE path: movieDataSize instead of bufsize*10.
    THPPlayerClose();
    THPPlayerOpen("movie.thp", TRUE);
    ActivePlayer.videoInfo.xSize       = 64;
    ActivePlayer.videoInfo.ySize       = 64;
    ActivePlayer.header.bufsize        = 128;
    ActivePlayer.header.movieDataSize  = 512;
    ActivePlayer.header.audioMaxSamples = 0;
    ActivePlayer.audioExist = 0;
    u32 sz_mem = THPPlayerCalcNeedMemory();
    // read: align32(512) = 512 (movieDataSize, onMemory=TRUE)
    // Y+U+V+page: 12288+3072+3072+4096 = 22528
    // total: 512+22528 = 23040
    chki((long)sz_mem, 23040L, "CalcNeedMemory onMemory movieDataSize=512");
}

// ---------------------------------------------------------------------------
// test_set_volume
// SetVolume is guarded by open && audioExist.
// Clamps vol [0..127] and duration [0..60000].
// Ramp: rampCount = 48 * duration; deltaVolume = (target - cur) / rampCount.
// Instant (duration=0): curVolume snaps to target immediately.
// ---------------------------------------------------------------------------
static void test_set_volume() {
    std::printf("[test_set_volume]\n");
    reset_player();

    // Not open → FALSE.
    chkb(THPPlayerSetVolume(64, 0) == FALSE, true, "SetVolume fails when not open");

    THPPlayerOpen("movie.thp", FALSE);
    // audioExist=0 → FALSE.
    chkb(THPPlayerSetVolume(64, 0) == FALSE, true, "SetVolume fails when audioExist=0");

    ActivePlayer.audioExist = 1;
    ActivePlayer.curVolume    = 0.0f;
    ActivePlayer.targetVolume = 0.0f;

    // Instant set (duration=0): curVolume should snap to vol immediately.
    BOOL ok = THPPlayerSetVolume(64, 0);
    chkb(ok == TRUE, true, "SetVolume(64,0) returns TRUE");
    chkf(ActivePlayer.targetVolume, 64.0f, "targetVolume=64 after SetVolume(64,0)");
    chkf(ActivePlayer.curVolume,    64.0f, "curVolume snaps to 64 (duration=0)");
    chki(ActivePlayer.rampCount, 0, "rampCount=0 for instant set");

    // Ramp: from cur=64 to target=0 over duration=10 frames.
    // rampCount = 48 * 10 = 480
    // deltaVolume = (0 - 64) / 480 = -0.13333...
    ActivePlayer.curVolume = 64.0f;
    ok = THPPlayerSetVolume(0, 10);
    chkb(ok == TRUE, true, "SetVolume(0,10) returns TRUE");
    chkf(ActivePlayer.targetVolume, 0.0f, "targetVolume=0 after SetVolume(0,10)");
    chki(ActivePlayer.rampCount, 480, "rampCount = 48*10 = 480");
    chkf(ActivePlayer.deltaVolume, (0.0f - 64.0f) / 480.0f,
         "deltaVolume = (0-64)/480", 1e-5f);

    // Clamp: vol > 127 → 127.
    ok = THPPlayerSetVolume(200, 0);
    chkb(ok == TRUE, true, "SetVolume(200,0) succeeds (clamped)");
    chkf(ActivePlayer.targetVolume, 127.0f, "vol 200 clamped to 127");

    // Clamp: vol < 0 → 0.
    ok = THPPlayerSetVolume(-5, 0);
    chkb(ok == TRUE, true, "SetVolume(-5,0) succeeds (clamped)");
    chkf(ActivePlayer.targetVolume, 0.0f, "vol -5 clamped to 0");

    // Clamp: duration > 60000 → 60000.
    // rampCount = 48 * 60000 = 2880000
    ActivePlayer.curVolume = 0.0f;
    ok = THPPlayerSetVolume(127, 99999);
    chkb(ok == TRUE, true, "SetVolume(127,99999) succeeds (duration clamped)");
    chki(ActivePlayer.rampCount, 48 * 60000, "duration clamped to 60000 → rampCount=2880000");
}

// ---------------------------------------------------------------------------
// test_draw_returns_minus_one
// DrawCurrentFrame must return -1 (no frame) on native PC (documented no-op).
// DrawDone must not crash.
// ---------------------------------------------------------------------------
static void test_draw_returns_minus_one() {
    std::printf("[test_draw_returns_minus_one]\n");
    reset_player();
    THPPlayerOpen("movie.thp", FALSE);
    THPPlayerPrepare(0, 0, 0);

    // No GX FIFO → always returns -1 (matches the decomp's null-dispTextureSet branch).
    s32 frame = THPPlayerDrawCurrentFrame(nullptr, 0, 0, 640, 480);
    chki((long)frame, -1L, "DrawCurrentFrame returns -1 (no frame available on PC)");

    // DrawDone must not crash.
    THPPlayerDrawDone();
    chki(1, 1, "DrawDone completes without crash");
}

// ---------------------------------------------------------------------------
// test_get_total_frame
// ---------------------------------------------------------------------------
static void test_get_total_frame() {
    std::printf("[test_get_total_frame]\n");
    reset_player();

    // Not open → 0.
    chki((long)THPPlayerGetTotalFrame(), 0L, "GetTotalFrame=0 when not open");

    THPPlayerOpen("movie.thp", FALSE);
    ActivePlayer.header.numFrames = 300;
    chki((long)THPPlayerGetTotalFrame(), 300L, "GetTotalFrame=300");
}

// ---------------------------------------------------------------------------
// test_quit_reinit
// Quit clears Initialized; Init after Quit succeeds; Open without Init fails.
// ---------------------------------------------------------------------------
static void test_quit_reinit() {
    std::printf("[test_quit_reinit]\n");

    THPPlayerInit();
    THPPlayerQuit();

    // After Quit, Open must fail (Initialized=FALSE).
    BOOL opened = THPPlayerOpen("movie.thp", FALSE);
    chkb(opened == FALSE, true, "Open fails after Quit (not Initialized)");

    // Re-init restores functionality.
    THPPlayerInit();
    opened = THPPlayerOpen("movie.thp", FALSE);
    chkb(opened == TRUE, true, "Open succeeds after re-Init");
    THPPlayerClose();
}

// ---------------------------------------------------------------------------
// test_set_buffer
// Verify SetBuffer slices the pointer correctly and records thpWork.
// Use a tiny stack buffer; only the pointer arithmetic matters (no real decode).
// ---------------------------------------------------------------------------
static void test_set_buffer() {
    std::printf("[test_set_buffer]\n");
    reset_player();

    THPPlayerOpen("movie.thp", FALSE);
    // Seed geometry: 16×16 video, bufsize=64, no audio.
    ActivePlayer.videoInfo.xSize = 16;
    ActivePlayer.videoInfo.ySize = 16;
    ActivePlayer.header.bufsize  = 64;
    ActivePlayer.audioExist      = 0;

    // Must fail when state != 0 (prepare first, then try).
    THPPlayerPrepare(0, 0, 0);
    static u8 buf[65536];
    BOOL r = THPPlayerSetBuffer(buf);
    chkb(r == FALSE, true, "SetBuffer fails when state=1 (must be state=0)");
    THPPlayerStop();  // back to state=0

    // Now SetBuffer should succeed.
    r = THPPlayerSetBuffer(buf);
    chkb(r == TRUE, true, "SetBuffer succeeds at state=0");

    // Verify that thpWork is past all the slices.
    // read: align32(64)*10 = 640; Y*3=align32(256)*3=768; U*3=align32(64)*3=192; V*3=192
    // total consumed = 640+768+192+192 = 1792
    u8* expected_work = buf + 1792;
    chkb(ActivePlayer.thpWork == expected_work, true,
         "thpWork points past all slices (1792 bytes in)");

    // Verify readBuffer[0] starts at buf.
    chkb(ActivePlayer.readBuffer[0].ptr == buf, true, "readBuffer[0].ptr = buf start");

    // Verify textureSet Y textures are consecutive (ysize=align32(256)=256 each).
    chkb(ActivePlayer.textureSet[0].ytexture == buf + 640, true,
         "textureSet[0].ytexture starts after read buffers");
    chkb(ActivePlayer.textureSet[1].ytexture ==
         ActivePlayer.textureSet[0].ytexture + 256 + 64 + 64,  // Y+U+V of set 0
         true, "textureSet[1].ytexture follows set[0] Y+U+V");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    test_init();
    test_open_close();
    test_prepare();
    test_prepare_seek_guard();
    test_play_stop_pause();
    test_play_guard();
    test_stop_guard();
    test_get_video_audio_info();
    test_calc_need_memory();
    test_set_volume();
    test_draw_returns_minus_one();
    test_get_total_frame();
    test_quit_reinit();
    test_set_buffer();

    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    if (g_fail == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    return 1;
}
