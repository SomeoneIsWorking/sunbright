// thp_test.cpp — unit tests for the native THP player (thp_impl.cpp + thp_pipeline.cpp).
//
// The real THPPlayerOpen now demuxes a THP off the DVD seam, so these tests install a
// synthetic in-memory THP disc and exercise the REAL big-endian demux path. The pure,
// deterministic units are asserted against hand-derived ground truth (decomp
// semantics): the demux, CalcNeedMemory/SetBuffer arithmetic, SetVolume ramp, the
// state guards, and the ADPCM audio codec (THPAudioDecode). The threaded decode
// pipeline + end-of-movie state machine are integration-verified live (SB_MOVIE_DBG),
// not here — THPPlayerPrepare spins real worker threads and blocks on the pipeline.
//
// State values (from the decomp):
//   0 = Stop/Closed   1 = Prepared   2 = Playing   3 = Done
//   4 = Paused        5 = Error

#include <THPPlayer/THPPlayer.h>
#include <dolphin/thp.h>
#include "../dvd_disc.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// Tiny assert harness
// ---------------------------------------------------------------------------
static int g_fail = 0, g_checks = 0;
static void chki(long got, long want, const char* what) {
    ++g_checks;
    if (got != want) { ++g_fail; std::printf("  FAIL: %s  got=%ld  want=%ld\n", what, got, want); }
}
static void chkb(bool got, bool want, const char* what) { chki((long)got, (long)want, what); }
static void chkf(float got, float want, const char* what, float eps = 1e-4f) {
    ++g_checks;
    if (std::fabs(got - want) > eps) { ++g_fail; std::printf("  FAIL: %s  got=%.6f  want=%.6f\n", what, got, want); }
}

// ---------------------------------------------------------------------------
// Synthetic THP disc — a minimal valid THP file served through the DVD seam so
// THPPlayerOpen("movie.thp") runs its real demux. Big-endian on disc.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> g_disc;   // the THP file bytes (disc offset 0)

static void put_be32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
    b[o] = v >> 24; b[o+1] = v >> 16; b[o+2] = v >> 8; b[o+3] = v;
}
static void put_bef32(std::vector<uint8_t>& b, size_t o, float f) {
    uint32_t v; std::memcpy(&v, &f, 4); put_be32(b, o, v);
}

static bool disc_read(void* dst, u32 len, u32 off, void* /*user*/) {
    if ((size_t)off + len > g_disc.size()) return false;
    std::memcpy(dst, g_disc.data() + off, len);
    return true;
}

// Known demux ground-truth values.
enum {
    THP_NUMFRAMES = 42, THP_BUFSIZE = 128, THP_AUDIOMAX = 256,
    THP_FIRSTFRAME = 100, THP_XSIZE = 64, THP_YSIZE = 48,
    THP_SNDCH = 2, THP_SNDFREQ = 32000, THP_SNDSAMP = 1000, THP_SNDTRK = 2,
    THP_COMPOFF = 0x40, THP_MOVIEOFF = 0x70,
};

static void install_synthetic_disc() {
    // --- THP file blob (0x80 bytes) ---
    g_disc.assign(0x80, 0);
    std::memcpy(&g_disc[0], "THP\0", 4);
    put_be32(g_disc, 4,  0x11000);        // version
    put_be32(g_disc, 8,  THP_BUFSIZE);    // bufsize
    put_be32(g_disc, 12, THP_AUDIOMAX);   // audioMaxSamples
    put_bef32(g_disc, 16, 30.0f);         // frameRate
    put_be32(g_disc, 20, THP_NUMFRAMES);  // numFrames
    put_be32(g_disc, 24, THP_FIRSTFRAME); // firstFrameSize
    put_be32(g_disc, 28, 0);              // movieDataSize
    put_be32(g_disc, 32, THP_COMPOFF);    // compInfoDataOffsets
    put_be32(g_disc, 36, 0);              // offsetDataOffsets
    put_be32(g_disc, 40, THP_MOVIEOFF);   // movieDataOffsets
    put_be32(g_disc, 44, 0);              // finalFrameDataOffsets
    // compInfo @ 0x40: numComponents=2, frameComp[0]=0(video), [1]=1(audio)
    put_be32(g_disc, THP_COMPOFF, 2);
    g_disc[THP_COMPOFF + 4 + 0] = 0;
    g_disc[THP_COMPOFF + 4 + 1] = 1;
    // video info @ 0x54 (compOff + sizeof(THPFrameCompInfo)=20)
    size_t vo = THP_COMPOFF + 20;
    put_be32(g_disc, vo + 0, THP_XSIZE);
    put_be32(g_disc, vo + 4, THP_YSIZE);
    put_be32(g_disc, vo + 8, 0);          // videoType
    // audio info @ 0x60 (vo + sizeof(THPVideoInfo)=12)
    size_t ao = vo + 12;
    put_be32(g_disc, ao + 0,  THP_SNDCH);
    put_be32(g_disc, ao + 4,  THP_SNDFREQ);
    put_be32(g_disc, ao + 8,  THP_SNDSAMP);
    put_be32(g_disc, ao + 12, THP_SNDTRK);

    // --- FST with one file entry "movie.thp" at disc offset 0 ---
    static std::vector<uint8_t> fst;
    const char* nm = "movie.thp";
    fst.assign(24, 0);                    // 2 entries * 12
    // entry0: root dir, next=2
    fst[0] = 1; put_be32(fst, 8, 2);
    // entry1: file, nameOff=1, fileOff=0, len=disc size
    put_be32(fst, 12, 1);                 // type=0 (byte0=0) | nameOff=1
    put_be32(fst, 16, 0);                 // disc offset
    put_be32(fst, 20, (u32)g_disc.size());
    // string table: [0]='\0' (root), [1..]="movie.thp\0"
    fst.push_back(0);
    for (const char* p = nm; *p; ++p) fst.push_back((uint8_t)*p);
    fst.push_back(0);

    sb_dvd_set_disc_source(disc_read, nullptr);
    sb_dvd_set_fst(fst.data(), (u32)fst.size());
}

static void reset_player() {
    THPPlayerQuit();
    THPPlayerInit();
}

// ---------------------------------------------------------------------------
// test_init
// ---------------------------------------------------------------------------
static void test_init() {
    std::printf("[test_init]\n");
    BOOL ok = THPPlayerInit();
    chkb(ok == TRUE, true, "Init returns TRUE");
    chki(THPPlayerGetState(), 0, "state after Init = 0 (Stop)");
    chkb(ActivePlayer.open == FALSE, true, "open=FALSE after Init");
}

// ---------------------------------------------------------------------------
// test_demux — the REAL big-endian demux of the synthetic THP.
// ---------------------------------------------------------------------------
static void test_demux() {
    std::printf("[test_demux]\n");
    reset_player();
    BOOL opened = THPPlayerOpen("movie.thp", FALSE);
    chkb(opened == TRUE, true, "Open demuxes synthetic THP");
    chki((long)ActivePlayer.header.version, 0x11000, "header.version");
    chki((long)ActivePlayer.header.numFrames, THP_NUMFRAMES, "header.numFrames (BE)");
    chki((long)ActivePlayer.header.bufsize, THP_BUFSIZE, "header.bufsize (BE)");
    chki((long)ActivePlayer.header.firstFrameSize, THP_FIRSTFRAME, "header.firstFrameSize (BE)");
    chki((long)ActivePlayer.header.movieDataOffsets, THP_MOVIEOFF, "header.movieDataOffsets (BE)");
    chkf(ActivePlayer.header.frameRate, 30.0f, "header.frameRate (BE f32)");
    chki((long)ActivePlayer.compInfo.numComponents, 2, "compInfo.numComponents");
    chki((long)ActivePlayer.videoInfo.xSize, THP_XSIZE, "videoInfo.xSize (BE)");
    chki((long)ActivePlayer.videoInfo.ySize, THP_YSIZE, "videoInfo.ySize (BE)");
    chki((long)ActivePlayer.videoInfo.videoType, 0, "videoInfo.videoType (BE)");
    chkb(ActivePlayer.audioExist == 1, true, "audioExist set from component[1]");
    chki((long)ActivePlayer.audioInfo.sndChannels, THP_SNDCH, "audioInfo.sndChannels (BE)");
    chki((long)ActivePlayer.audioInfo.sndFrequency, THP_SNDFREQ, "audioInfo.sndFrequency (BE)");
    chki((long)ActivePlayer.audioInfo.sndNumTracks, THP_SNDTRK, "audioInfo.sndNumTracks (BE)");
    THPPlayerClose();
}

// ---------------------------------------------------------------------------
// test_open_close — open guard, double-open, close guard.
// ---------------------------------------------------------------------------
static void test_open_close() {
    std::printf("[test_open_close]\n");
    reset_player();
    BOOL opened = THPPlayerOpen("movie.thp", FALSE);
    chkb(opened == TRUE, true, "Open returns TRUE");
    chki(THPPlayerGetState(), 0, "state after Open = 0");
    chkf(ActivePlayer.curVolume, 127.0f, "curVolume initialised to 127");
    chkb(THPPlayerOpen("movie.thp", FALSE) == FALSE, true, "second Open fails (already open)");
    chkb(THPPlayerClose() == TRUE, true, "Close returns TRUE");
    chkb(ActivePlayer.open == FALSE, true, "open=FALSE after Close");
    chkb(THPPlayerClose() == FALSE, true, "second Close fails (not open)");
}

// ---------------------------------------------------------------------------
// test_open_guards — Open fails when not Initialized; bad path fails.
// ---------------------------------------------------------------------------
static void test_open_guards() {
    std::printf("[test_open_guards]\n");
    THPPlayerInit();
    chkb(THPPlayerOpen("nonexistent.thp", FALSE) == FALSE, true, "Open(missing file) fails");
    THPPlayerQuit();
    chkb(THPPlayerOpen("movie.thp", FALSE) == FALSE, true, "Open fails after Quit (not Initialized)");
}

// ---------------------------------------------------------------------------
// test_prepare_seek_guard — guard checks that return before any thread is created.
// ---------------------------------------------------------------------------
static void test_prepare_seek_guard() {
    std::printf("[test_prepare_seek_guard]\n");
    reset_player();
    THPPlayerOpen("movie.thp", FALSE);

    ActivePlayer.header.offsetDataOffsets = 0;
    BOOL r = THPPlayerPrepare(5, 0, 0);
    chkb(r == FALSE, true, "Prepare(frame>0) with offsetDataOffsets==0 fails");

    ActivePlayer.header.offsetDataOffsets = 0x1000;
    r = THPPlayerPrepare((s32)ActivePlayer.header.numFrames, 0, 0);
    chkb(r == FALSE, true, "Prepare(frame>=numFrames) fails");

    // Prepare without Open.
    THPPlayerClose();
    r = THPPlayerPrepare(0, 0, 0);
    chkb(r == FALSE, true, "Prepare without Open fails");
}

// ---------------------------------------------------------------------------
// test_play_stop_pause — state-machine guards. We set state directly (Prepare spins
// real threads), matching the decomp's state values: Prepared(1)->Playing(2)->...
// ---------------------------------------------------------------------------
static void test_play_stop_pause() {
    std::printf("[test_play_stop_pause]\n");
    reset_player();
    THPPlayerOpen("movie.thp", FALSE);

    ActivePlayer.state = 1;  // simulate Prepared (post-Prepare)
    chkb(THPPlayerPlay() == TRUE, true, "Play() from Prepared returns TRUE");
    chki(THPPlayerGetState(), 2, "state after Play = 2 (Playing)");
    chki((long)ActivePlayer.retaceCount, -1, "retaceCount = -1 by Play");

    chkb(THPPlayerPause() == TRUE, true, "Pause() from Playing returns TRUE");
    chki(THPPlayerGetState(), 4, "state after Pause = 4 (Paused)");
    chki(ActivePlayer.internalState, 4, "internalState=4 after Pause");
    chkb(THPPlayerPause() == FALSE, true, "second Pause fails (not Playing)");

    chkb(THPPlayerPlay() == TRUE, true, "Play() from Paused returns TRUE");
    chki(THPPlayerGetState(), 2, "state after Play from Paused = 2");

    THPPlayerStop();
    chki(THPPlayerGetState(), 0, "state after Stop = 0");
    chki(ActivePlayer.internalState, 0, "internalState=0 after Stop");
    chki(ActivePlayer.videoError, 0, "videoError cleared by Stop");
}

// ---------------------------------------------------------------------------
// test_play_guard — Play fails when state is not 1 or 4.
// ---------------------------------------------------------------------------
static void test_play_guard() {
    std::printf("[test_play_guard]\n");
    reset_player();
    chkb(THPPlayerPlay() == FALSE, true, "Play without open fails");
    THPPlayerOpen("movie.thp", FALSE);
    chkb(THPPlayerPlay() == FALSE, true, "Play from state=0 (not Prepared) fails");
    THPPlayerClose();
}

// ---------------------------------------------------------------------------
// test_calc_need_memory — pure arithmetic (hand-derived).
// ---------------------------------------------------------------------------
static void test_calc_need_memory() {
    std::printf("[test_calc_need_memory]\n");
    reset_player();
    chki((long)THPPlayerCalcNeedMemory(), 0L, "CalcNeedMemory=0 when not open");

    THPPlayerOpen("movie.thp", FALSE);
    ActivePlayer.videoInfo.xSize = 64;
    ActivePlayer.videoInfo.ySize = 64;
    ActivePlayer.header.bufsize  = 128;
    ActivePlayer.header.audioMaxSamples = 256;
    ActivePlayer.audioExist = 0;
    // read 1280 + Y 12288 + U 3072 + V 3072 + page 4096 = 23808
    chki((long)THPPlayerCalcNeedMemory(), 23808L, "CalcNeedMemory no-audio");
    ActivePlayer.audioExist = 1;
    chki((long)THPPlayerCalcNeedMemory(), 26880L, "CalcNeedMemory with-audio (+3072)");
    THPPlayerClose();
}

// ---------------------------------------------------------------------------
// test_set_buffer — pointer slicing (state must be 0).
// ---------------------------------------------------------------------------
static void test_set_buffer() {
    std::printf("[test_set_buffer]\n");
    reset_player();
    THPPlayerOpen("movie.thp", FALSE);
    ActivePlayer.videoInfo.xSize = 16;
    ActivePlayer.videoInfo.ySize = 16;
    ActivePlayer.header.bufsize  = 64;
    ActivePlayer.audioExist      = 0;

    ActivePlayer.state = 1;  // not 0 -> must fail
    static u8 buf[65536];
    chkb(THPPlayerSetBuffer(buf) == FALSE, true, "SetBuffer fails when state!=0");
    ActivePlayer.state = 0;

    chkb(THPPlayerSetBuffer(buf) == TRUE, true, "SetBuffer succeeds at state=0");
    // read align32(64)*10=640; Y align32(256)*3=768; U align32(64)*3=192; V=192 -> 1792
    chkb(ActivePlayer.thpWork == buf + 1792, true, "thpWork past all slices (1792)");
    chkb(ActivePlayer.readBuffer[0].ptr == buf, true, "readBuffer[0].ptr = buf start");
    chkb(ActivePlayer.textureSet[0].ytexture == buf + 640, true, "textureSet[0].ytexture after read bufs");
    THPPlayerClose();
}

// ---------------------------------------------------------------------------
// test_set_volume — clamp + ramp arithmetic.
// ---------------------------------------------------------------------------
static void test_set_volume() {
    std::printf("[test_set_volume]\n");
    reset_player();
    chkb(THPPlayerSetVolume(64, 0) == FALSE, true, "SetVolume fails when not open");
    THPPlayerOpen("movie.thp", FALSE);
    ActivePlayer.audioExist = 1;
    ActivePlayer.curVolume = 0.0f; ActivePlayer.targetVolume = 0.0f;
    chkb(THPPlayerSetVolume(64, 0) == TRUE, true, "SetVolume(64,0) TRUE");
    chkf(ActivePlayer.curVolume, 64.0f, "curVolume snaps to 64 (duration=0)");
    chki(ActivePlayer.rampCount, 0, "rampCount=0 for instant set");
    ActivePlayer.curVolume = 64.0f;
    chkb(THPPlayerSetVolume(0, 10) == TRUE, true, "SetVolume(0,10) TRUE");
    chki(ActivePlayer.rampCount, 480, "rampCount = 48*10 = 480");
    chkf(ActivePlayer.deltaVolume, (0.0f - 64.0f) / 480.0f, "deltaVolume=(0-64)/480", 1e-5f);
    chkb(THPPlayerSetVolume(200, 0) == TRUE, true, "SetVolume(200,0) clamps");
    chkf(ActivePlayer.targetVolume, 127.0f, "vol 200 clamped to 127");
    THPPlayerClose();
}

// ---------------------------------------------------------------------------
// test_draw / total frame.
// ---------------------------------------------------------------------------
static void test_draw_total() {
    std::printf("[test_draw_total]\n");
    reset_player();
    chki((long)THPPlayerGetTotalFrame(), 0L, "GetTotalFrame=0 when not open");
    THPPlayerOpen("movie.thp", FALSE);
    chki((long)THPPlayerGetTotalFrame(), THP_NUMFRAMES, "GetTotalFrame from header");
    s32 frame = THPPlayerDrawCurrentFrame(nullptr, 0, 0, 640, 480);
    chki((long)frame, -1L, "DrawCurrentFrame returns -1 (STAGE B not drawn yet)");
    THPPlayerDrawDone();   // must not crash
    THPPlayerClose();
}

// ---------------------------------------------------------------------------
// test_adpcm — THPAudioDecode (ADPCM) on a hand-built record.
// With predictor=0, scale=0, all coefficients 0, yn1=yn2=0, the recurrence collapses
// to out[i] = signed4(nibble[i]) (see derivation in the test body).
// ---------------------------------------------------------------------------
extern "C" u32 THPAudioDecode(s16* audioBuffer, u8* audioFrame, s32 flag);

static void test_adpcm() {
    std::printf("[test_adpcm]\n");
    // 80-byte BE record header (offsetNextChannel=0, sampleSize=4, coefs/yns=0).
    u8 frame[80 + 8];
    std::memset(frame, 0, sizeof(frame));
    // offsetNextChannel @0 = 0 (mono); sampleSize @4 = 4 (BE)
    frame[4] = 0; frame[5] = 0; frame[6] = 0; frame[7] = 4;
    // nibble stream @ offset 80: byte0 = predictor/scale = 0x00;
    //   byte1 = 0x12 (nibbles 1,2); byte2 = 0x7F (nibbles 7, -1).
    frame[80] = 0x00;
    frame[81] = 0x12;
    frame[82] = 0x7F;

    s16 out[16];
    std::memset(out, 0xAB, sizeof(out));
    u32 n = THPAudioDecode(out, frame, 0);
    chki((long)n, 4, "THPAudioDecode returns sampleSize=4");
    // flag=0 -> interleaved L/R identical (offsetNextChannel==0 branch writes both).
    chki(out[0], 1,  "sample0 L = 1");
    chki(out[1], 1,  "sample0 R = 1");
    chki(out[2], 2,  "sample1 = 2");
    chki(out[4], 7,  "sample2 = 7");
    chki(out[6], -1, "sample3 = -1 (sign-extended low nibble)");
}

int main() {
    install_synthetic_disc();
    test_init();
    test_demux();
    test_open_close();
    test_open_guards();
    test_prepare_seek_guard();
    test_play_stop_pause();
    test_play_guard();
    test_calc_need_memory();
    test_set_buffer();
    test_set_volume();
    test_draw_total();
    test_adpcm();

    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    if (g_fail == 0) { std::printf("ALL PASS\n"); return 0; }
    return 1;
}
