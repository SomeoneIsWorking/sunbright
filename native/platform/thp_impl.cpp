// thp_impl.cpp — native PC seam for the THP (FMV movie player) SDK surface.
//
// Ported from: reference/sms/src/THPPlayer/THPPlayer.c
// Header:      reference/sms/include/THPPlayer/THPPlayer.h
//
// DESIGN BOUNDARY
// ---------------
// THP frame DECODING (JPEG-like DCT video, ADPCM audio) and DVD streaming are NOT
// implemented here — they require the hardware decoder or a full software codec, which
// is a large subsystem and out of scope for the bring-up seam. What IS faithfully
// ported:
//
//   • The THPPlayer global struct (ActivePlayer) with all state fields.
//   • The FULL state machine (Init/Open/Close/Prepare/Play/Stop/Pause/Quit), mirroring
//     the exact state field values and guard conditions from the decomp.
//   • THPPlayerGetState / GetVideoInfo / GetAudioInfo / CalcNeedMemory / SetBuffer /
//     SetVolume — pure struct operations, fully faithful.
//   • PrepareReady — provided as a no-op signal sink (the threading context is absent).
//
// DOCUMENTED NO-OPS (why):
//   • THPPlayerDrawCurrentFrame — the GX YUV->RGB draw path requires texture upload to
//     the GX FIFO and GX state setup (THPGXYuv2RgbSetup/Draw/Restore). There is no
//     native GX command stream in the PC engine; the ngx renderer handles frames
//     differently. Returns -1 (no frame, as the decomp does when dispTextureSet==NULL).
//   • THPPlayerDrawDone — calls GXDrawDone() which syncs the GX FIFO. No-op on PC;
//     the native renderer has its own present-sync.
//
// State values (faithful to the decomp):
//   0 = Stop/Closed  1 = Prepared  2 = Playing  3 = Done  4 = Paused  5 = Error

#include <THPPlayer/THPPlayer.h>
#include <cstring>

// ---------------------------------------------------------------------------
// Module-level statics (mirrors THPPlayer.c)
// ---------------------------------------------------------------------------

// The single active player instance (matches `extern THPPlayer ActivePlayer` in header).
THPPlayer ActivePlayer;

// Whether THPPlayerInit() has been called successfully.
static BOOL Initialized = FALSE;

// Volume lookup table (128 entries, verbatim from the decomp).
// Index = integer volume [0..127]; value = fixed-point attenuation (Q15 scale).
// Entry 127 = -32768 (u16 wrap = full volume 32768).
static u16 VolumeTable[128] = {
    0, 2, 8, 18, 32, 50, 73, 99, 130,
    164, 203, 245, 292, 343, 398, 457, 520,
    587, 658, 733, 812, 895, 983, 1074, 1170,
    1269, 1373, 1481, 1592, 1708, 1828, 1952, 2080,
    2212, 2348, 2488, 2632, 2781, 2933, 3090, 3250,
    3415, 3583, 3756, 3933, 4114, 4298, 4487, 4680,
    4877, 5079, 5284, 5493, 5706, 5924, 6145, 6371,
    6600, 6834, 7072, 7313, 7559, 7809, 8063, 8321,
    8583, 8849, 9119, 9394, 9672, 9954, 10241, 10531,
    10826, 11125, 11427, 11734, 12045, 12360, 12679, 13002,
    13329, 13660, 13995, 14335, 14678, 15025, 15377, 15732,
    16092, 16456, 16823, 17195, 17571, 17951, 18335, 18723,
    19115, 19511, 19911, 20316, 20724, 21136, 21553, 21974,
    22398, 22827, 23260, 23696, 24137, 24582, 25031, 25484,
    25941, 26402, 26868, 27337, 27810, 28288, 28769, 29255,
    29744, 30238, 30736, 31238, 31744, 32254,
    static_cast<u16>(-32768)  // index 127 = 32768 in u16
};

// ---------------------------------------------------------------------------
// Alignment helper (mirrors ALIGN_NEXT(x,align) from the decomp)
// ---------------------------------------------------------------------------
static inline u32 align_next(u32 x, u32 align) {
    return (x + align - 1) & ~(align - 1);
}

// ---------------------------------------------------------------------------
// extern "C" API — exact signatures from THPPlayer.h
// ---------------------------------------------------------------------------
extern "C" {

// THPPlayerInit — initialise the THP subsystem.
// Faithful port: zeroes ActivePlayer, sets Initialized=TRUE.
// GC-specific skipped: LCEnable (locked L1 cache), OSInitMessageQueue,
// THPInit (the hardware THP decoder init), audio callback registration,
// DCFlushRange (cache maintenance). None of these exist on native PC.
BOOL THPPlayerInit()
{
    memset(&ActivePlayer, 0, sizeof(THPPlayer));
    Initialized = TRUE;
    return TRUE;
}

// THPPlayerQuit — shut down the THP subsystem.
// Faithful port: clears Initialized.
// GC-specific skipped: LCDisable, audio callback unregistration.
void THPPlayerQuit()
{
    Initialized = FALSE;
}

// THPPlayerOpen — open a THP file and parse its header.
// State machine guard: requires Initialized and !open.
// Faithful port: validates preconditions, sets videoInfo/audioInfo/state fields.
// GC-specific skipped: DVDOpen/DVDReadPrio (disc read), THPHeader magic/version
// check from disc, audioExist from component scan. On PC there is no disc and no
// THP decoder; we accept the call, record the filename (unused), set state=0, open=TRUE.
// VideoInfo/AudioInfo remain zeroed unless the caller uses SetBuffer/Prepare with
// real data — in practice the game calls Open then Prepare then Play, and if we
// have no real file, DrawCurrentFrame returns -1 (no frame to display).
// This is HONEST: the player is open but has no decoded content.
BOOL THPPlayerOpen(const char* /*fileName*/, BOOL onMemory)
{
    if (Initialized == FALSE) return FALSE;
    if (ActivePlayer.open)   return FALSE;

    memset(&ActivePlayer.videoInfo, 0, sizeof(THPVideoInfo));
    memset(&ActivePlayer.audioInfo, 0, sizeof(THPAudioInfo));

    ActivePlayer.internalState = 0;
    ActivePlayer.state         = 0;
    ActivePlayer.playFlag      = 0;
    ActivePlayer.onMemory      = onMemory;
    ActivePlayer.open          = TRUE;
    ActivePlayer.audioExist    = 0;

    ActivePlayer.curVolume    = 127.0f;
    ActivePlayer.targetVolume = 127.0f;
    ActivePlayer.rampCount    = 0;

    return TRUE;
}

// THPPlayerClose — close the player.
// Faithful port: guard = open && state==0 (i.e. stopped).
// GC-specific skipped: DVDClose.
BOOL THPPlayerClose()
{
    if (ActivePlayer.open && ActivePlayer.state == 0) {
        ActivePlayer.open = FALSE;
        return TRUE;
    }
    return FALSE;
}

// THPPlayerPrepare — prepare playback, optionally seeking to a frame.
// State machine: open && state==0 → state=1.
// Faithful port: validates frame-seek preconditions (offsetDataOffsets, numFrames),
// records initOffset/initReadSize/initReadFrame from header fields, sets playFlag,
// audioTrack, resets counters, transitions state to 1 (Prepared).
// GC-specific skipped: DVDReadPrio for offset table, CreateVideoDecodeThread,
// CreateAudioDecodeThread, CreateReadThread, thread-start, WaitUntilPrepare,
// VISetPostRetraceCallback. On PC there is no decode thread; we transition directly
// to state=1 without a decode-ready wait.
BOOL THPPlayerPrepare(s32 frame, u8 flag, s32 audioTrack)
{
    if (!(ActivePlayer.open && ActivePlayer.state == 0)) return FALSE;

    // Validate frame seek (mirrors the decomp logic faithfully).
    if (frame > 0) {
        // Seeking requires an offset table and a valid frame index.
        if (ActivePlayer.header.offsetDataOffsets == 0) return FALSE;
        if ((u32)frame >= ActivePlayer.header.numFrames) return FALSE;
        // On GC we would DVD-read the offset; here we don't have the file.
        // Record initReadFrame so GetState / DrawCurrentFrame can use it.
        ActivePlayer.initOffset    = ActivePlayer.header.movieDataOffsets; // best-effort
        ActivePlayer.initReadFrame = frame;
        ActivePlayer.initReadSize  = 0;
    } else {
        ActivePlayer.initOffset    = ActivePlayer.header.movieDataOffsets;
        ActivePlayer.initReadSize  = ActivePlayer.header.firstFrameSize;
        ActivePlayer.initReadFrame = frame;
    }

    if (ActivePlayer.audioExist) {
        if (audioTrack < 0 || audioTrack >= (s32)ActivePlayer.audioInfo.sndNumTracks)
            return FALSE;
        ActivePlayer.curAudioTrack = audioTrack;
    }

    ActivePlayer.playFlag         = flag & 1;
    ActivePlayer.videoDecodeCount = 0;

    // State transitions (mirrors the decomp's post-WaitUntilPrepare block).
    ActivePlayer.state           = 1;  // Prepared
    ActivePlayer.internalState   = 0;
    ActivePlayer.dispTextureSet  = nullptr;
    ActivePlayer.playAudioBuffer = nullptr;
    ActivePlayer.curVideoNumber  = 0;
    ActivePlayer.curAudioNumber  = 0;

    return TRUE;
}

// THPPlayerPlay — start playback.
// State machine: open && (state==1 || state==4) → state=2.
// Faithful port (exact guard + counter reset from the decomp).
BOOL THPPlayerPlay()
{
    if (ActivePlayer.open &&
        (ActivePlayer.state == 1 || ActivePlayer.state == 4)) {
        ActivePlayer.state       = 2;  // Playing
        ActivePlayer.prevCount   = 0;
        ActivePlayer.curCount    = 0;
        ActivePlayer.retaceCount = -1;
        return TRUE;
    }
    return FALSE;
}

// THPPlayerStop — stop playback unconditionally.
// State machine: open && state!=0 → state=0.
// Faithful port: resets internalState, error flags, volume ramp.
// GC-specific skipped: VISetPostRetraceCallback, DVDCancel, ReadThreadCancel,
// VideoDecodeThreadCancel, AudioDecodeThreadCancel, PopUsedTextureSet (no decode threads).
void THPPlayerStop()
{
    if (ActivePlayer.open && ActivePlayer.state != 0) {
        ActivePlayer.internalState = 0;
        ActivePlayer.state         = 0;  // Stop

        ActivePlayer.curVolume  = ActivePlayer.targetVolume;
        ActivePlayer.rampCount  = 0;
        ActivePlayer.dvdError   = 0;
        ActivePlayer.videoError = 0;
    }
}

// THPPlayerPause — pause playback.
// State machine: open && state==2 → state=4.
// Faithful port (exact guard from the decomp).
BOOL THPPlayerPause()
{
    if (ActivePlayer.open && ActivePlayer.state == 2) {
        ActivePlayer.internalState = 4;
        ActivePlayer.state         = 4;  // Paused
        return TRUE;
    }
    return FALSE;
}

// THPPlayerSetBuffer — assign pre-allocated working memory to the player.
// State machine: open && state==0 only.
// Faithful port: slices the buffer into readBuffers, Y/U/V textureSets, audioBuffers,
// thpWork pointer — exactly as the decomp does with ptr arithmetic and ALIGN_NEXT(,32).
// GC-specific skipped: DCInvalidateRange (cache invalidation, no-op on PC).
BOOL THPPlayerSetBuffer(u8* buffer)
{
    if (!(ActivePlayer.open && ActivePlayer.state == 0)) return FALSE;

    u8* ptr = buffer;

    if (ActivePlayer.onMemory) {
        ActivePlayer.movieData = buffer;
        ptr += ActivePlayer.header.movieDataSize;
    } else {
        for (u32 i = 0; i < THP_READ_BUFFER_COUNT; i++) {
            ActivePlayer.readBuffer[i].ptr = ptr;
            ptr += align_next(ActivePlayer.header.bufsize, 32);
        }
    }

    u32 ysize  = align_next(
        ActivePlayer.videoInfo.xSize * ActivePlayer.videoInfo.ySize, 32);
    u32 uvsize = align_next(
        ActivePlayer.videoInfo.xSize * ActivePlayer.videoInfo.ySize / 4, 32);

    for (u32 i = 0; i < THP_TEXTURE_SET_COUNT; i++) {
        ActivePlayer.textureSet[i].ytexture = ptr; ptr += ysize;
        ActivePlayer.textureSet[i].utexture = ptr; ptr += uvsize;
        ActivePlayer.textureSet[i].vtexture = ptr; ptr += uvsize;
    }

    if (ActivePlayer.audioExist) {
        for (u32 i = 0; i < THP_AUDIO_BUFFER_COUNT; i++) {
            ActivePlayer.audioBuffer[i].buffer      = reinterpret_cast<s16*>(ptr);
            ActivePlayer.audioBuffer[i].curPtr      = reinterpret_cast<s16*>(ptr);
            ActivePlayer.audioBuffer[i].validSample = 0;
            ptr += align_next(ActivePlayer.header.audioMaxSamples * 4, 32);
        }
    }

    ActivePlayer.thpWork = ptr;
    return TRUE;
}

// THPPlayerCalcNeedMemory — compute the total buffer size THPPlayerSetBuffer needs.
// Faithful port of the exact formula from the decomp (ALIGN_NEXT(,32) on each slice).
// Returns 0 if not open (also matches the decomp).
u32 THPPlayerCalcNeedMemory()
{
    if (!ActivePlayer.open) return 0;

    u32 size = ActivePlayer.onMemory
        ? align_next(ActivePlayer.header.movieDataSize, 32)
        : align_next(ActivePlayer.header.bufsize, 32) * THP_READ_BUFFER_COUNT;

    // Three Y textures, three U textures, three V textures.
    u32 ysize  = align_next(
        ActivePlayer.videoInfo.xSize * ActivePlayer.videoInfo.ySize, 32);
    u32 uvsize = align_next(
        ActivePlayer.videoInfo.xSize * ActivePlayer.videoInfo.ySize / 4, 32);

    size += ysize  * THP_TEXTURE_SET_COUNT;
    size += uvsize * THP_TEXTURE_SET_COUNT;
    size += uvsize * THP_TEXTURE_SET_COUNT;

    if (ActivePlayer.audioExist) {
        size += align_next(ActivePlayer.header.audioMaxSamples * 4, 32)
                * THP_AUDIO_BUFFER_COUNT;
    }

    return size + 0x1000;  // extra page (verbatim from decomp)
}

// THPPlayerGetVideoInfo — copy videoInfo out of the player struct.
// Faithful port: guard = open; memcpy the THPVideoInfo sub-struct.
BOOL THPPlayerGetVideoInfo(THPVideoInfo* videoInfo)
{
    if (ActivePlayer.open) {
        memcpy(videoInfo, &ActivePlayer.videoInfo, sizeof(THPVideoInfo));
        return TRUE;
    }
    return FALSE;
}

// THPPlayerGetAudioInfo — copy audioInfo out of the player struct.
// Faithful port: guard = open; memcpy the THPAudioInfo sub-struct.
BOOL THPPlayerGetAudioInfo(THPAudioInfo* audioInfo)
{
    if (ActivePlayer.open) {
        memcpy(audioInfo, &ActivePlayer.audioInfo, sizeof(THPAudioInfo));
        return TRUE;
    }
    return FALSE;
}

// THPPlayerGetState — return the current player state.
// Faithful port: verbatim return of ActivePlayer.state (u8 promoted to s32).
// Values: 0=Stop 1=Prepared 2=Playing 3=Done 4=Paused 5=Error
s32 THPPlayerGetState()
{
    return (s32)ActivePlayer.state;
}

// THPPlayerSetVolume — set playback volume with an optional ramp duration.
// Faithful port: clamps vol [0..127] and duration [0..60000], computes rampCount
// and deltaVolume exactly as the decomp.
// GC-specific skipped: AIGetDSPSampleRate() — on GC, numSamples is 32 (32kHz) or
// 48 (48kHz). On native PC we always use 48 kHz audio; we use 48 as the constant.
// The ramp arithmetic (rampCount = numSamples * duration, deltaVolume = dv/rampCount)
// is otherwise identical.
BOOL THPPlayerSetVolume(s32 vol, s32 duration)
{
    if (!(ActivePlayer.open && ActivePlayer.audioExist)) return FALSE;

    // Clamp inputs (faithful to the decomp).
    if (vol > 127)    vol = 127;
    if (vol < 0)      vol = 0;
    if (duration > 60000) duration = 60000;
    if (duration < 0)     duration = 0;

    ActivePlayer.targetVolume = (f32)vol;
    if (duration != 0) {
        // GC chose 32 or 48 based on DSP sample rate; native PC is always 48 kHz.
        const s32 numSamples = 48;
        ActivePlayer.rampCount   = numSamples * duration;
        ActivePlayer.deltaVolume = (ActivePlayer.targetVolume - ActivePlayer.curVolume)
                                   / (f32)ActivePlayer.rampCount;
    } else {
        ActivePlayer.rampCount = 0;
        ActivePlayer.curVolume = ActivePlayer.targetVolume;
    }

    return TRUE;
}

// THPPlayerDrawCurrentFrame — draw the current decoded frame as a GX textured quad.
// DOCUMENTED NO-OP: the GX YUV→RGB draw path (THPGXYuv2RgbSetup/Draw/Restore) requires
// the GX command FIFO and GX texture-load hardware, which do not exist in the native PC
// engine. The ngx renderer handles THP frames via a separate path (when implemented).
// Returns -1 (= "no frame displayed") matching the decomp's dispTextureSet==NULL branch.
s32 THPPlayerDrawCurrentFrame(GXRenderModeObj* /*rmode*/, u32 /*x*/, u32 /*y*/,
                               u32 /*polygonW*/, u32 /*polygonH*/)
{
    // No GX FIFO on native PC; ngx will handle THP display when implemented.
    return -1;
}

// THPPlayerDrawDone — sync the GX FIFO after frame draw and recycle texture sets.
// DOCUMENTED NO-OP: GXDrawDone() syncs the GX command processor. No FIFO on PC.
// The texture-set recycling loop is also a no-op (no decode thread pushes to the
// UsedTextureSetQueue on PC). The ngx renderer manages its own frame lifecycle.
void THPPlayerDrawDone()
{
    // GXDrawDone() and texture-set recycling are GX/threading constructs.
    // No-op on native PC; ngx manages present sync independently.
}

// THPPlayerGetTotalFrame — return the total frame count from the THP header.
u32 THPPlayerGetTotalFrame()
{
    if (ActivePlayer.open) return ActivePlayer.header.numFrames;
    return 0;
}

// THPPlayerPostDrawDone — post-present texture-set recycle (called after rendering).
// DOCUMENTED NO-OP: same reason as DrawDone — no decode thread, no UsedTextureSetQueue.
void THPPlayerPostDrawDone()
{
    // No-op on native PC; decode threads are absent.
}

} // extern "C"

// PrepareReady — signal that the decode threads are ready (called by the read/decode
// threads in the decomp to unblock WaitUntilPrepare).
// On native PC there are no decode threads, so Prepare() transitions directly.
// THPPlayer.h declares PrepareReady() OUTSIDE the extern "C" guard (C++ linkage) —
// match that here so there is no linkage-conflict with the header declaration.
void PrepareReady(int /*msg*/)
{
    // No-op: decode threads don't exist on PC; Prepare() transitions state directly.
}
