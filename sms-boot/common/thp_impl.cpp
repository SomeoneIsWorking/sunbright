// thp_impl.cpp — native PC port of the THP (FMV movie) player core.
//
// Ported from: reference/sms/src/THPPlayer/THPPlayer.c (the player + state machine,
// PlayControl VI callback, audio mixer). The decode workers + codecs live in
// thp_pipeline.cpp. Together they replace the GC THP subsystem with a faithful PC
// implementation: real disc streaming (DVD seam), the real threaded decode pipeline
// (OS seam std::thread + OSMessageQueue), and the real end-of-movie state machine.
//
// WHAT IS FAITHFUL (this session, STAGE A — unblock the opening movie):
//   • THPPlayerOpen demuxes the REAL Entrance.thp off the disc (header + component
//     info), big-endian-swapped from the on-disc bytes.
//   • THPPlayerPrepare spins up the Read/Video/Audio decode threads and registers
//     PlayControl as the VI post-retrace callback — exactly as the decomp.
//   • PlayControl advances internalState/state and detects end-of-movie. For
//     Entrance.thp (audio present) the end gate is AUDIO-driven: when curAudioNumber
//     reaches numFrames and the last audio buffer drains, state -> 3 (Done), which is
//     what TMovieDirector::direct() waits on to reach decideNextMode -> GAMEPLAY.
//   • MixAudio / the ADPCM codec are real, so curAudioNumber advances on real data.
//
// NATIVE DEVIATION (documented, faithful in effect): on GameCube the THP audio is
// pulled by the AI DMA interrupt (audioCallback registered into JASDriver). sms-boot
// has no audio device yet, so a small host "audio-clock" thread (audio_pump) pulls
// samples at the 32 kHz device rate, driving MixAudio exactly like the AI interrupt
// would. SB_THP_FAST=1 runs it uncapped (test pacing) to reach end-of-movie quickly;
// default is real-time. Decoded PCM is currently discarded (no host sink in boot) —
// wiring it to the audio device is a later milestone; it is not on the unblock path.
//
// STAGE B (next): THPVideoDecode (the paired-single IDCT) + THPPlayerDrawCurrentFrame
// to put the decoded frames on screen (thp_pipeline.cpp has the stub + the hook).

#include <THPPlayer/THPPlayer.h>
#include <THPPlayer/THPRead.h>
#include <THPPlayer/THPVideoDecode.h>
#include <THPPlayer/THPAudioDecode.h>
#include <dolphin/thp.h>
#include <dolphin/os.h>
#include <dolphin/dvd.h>
#include <dolphin/vi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

// ---------------------------------------------------------------------------
// Module statics (mirror THPPlayer.c)
// ---------------------------------------------------------------------------
THPPlayer ActivePlayer;

static u32 WorkBuffer[16] __attribute__((aligned(32)));
static OSMessageQueue PrepareReadyQueue;
static OSMessageQueue UsedTextureSetQueue;
static OSMessage UsedTextureSetMessage[3];
static s16 SoundBuffer[2][1120] __attribute__((aligned(32)));

static BOOL Initialized;
static void* PrepareReadyMessage;
static VIRetraceCallback OldVIPostCallback;
static s32 SoundBufferIndex;

// Volume lookup table (128 entries, verbatim from the decomp).
u16 VolumeTable[128] = {
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
    static_cast<u16>(-32768)
};

#define ALIGN_NEXT(x, a) (((u32)(x) + ((a) - 1)) & ~((u32)(a) - 1))

// ---------------------------------------------------------------------------
// Big-endian helpers (on-disc THP data is big-endian).
// ---------------------------------------------------------------------------
static inline u32 rd_be32(const void* p) {
    const u8* b = (const u8*)p;
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}
static inline f32 rd_bef32(const void* p) {
    u32 v = rd_be32(p);
    f32 f; std::memcpy(&f, &v, 4); return f;
}

// ---------------------------------------------------------------------------
// Native audio clock (stand-in for the AI DMA interrupt; see file header).
//
// SYNCHRONOUS model: there are no real worker threads — everything runs on one
// cooperative thread (os_impl.cpp). The audio is therefore pulled once per VI
// retrace from PlayControl (audio_tick), feeding one NTSC field's worth of samples
// (32 kHz / 60 Hz, with the fractional remainder carried). Because both the audio
// drain AND the video frame advance happen per-retrace, the A/V end-of-movie gate
// stays in lockstep regardless of wall-clock pacing — so SB_THP_FAST (uncapped
// retrace) reaches end-of-movie quickly without desyncing audio from video.
// ---------------------------------------------------------------------------
static constexpr s32 kAudioSampleRate = 32000;  // THP audio device rate
static s32 g_audio_acc = 0;                      // fractional-sample carry (per field)
static s16* audioCallbackWithMSound(s32 p1);

static void audio_clock_reset() { g_audio_acc = 0; }

// Pull one VI field's worth of audio (advances curAudioNumber as buffers drain).
static void audio_tick() {
    if (!ActivePlayer.audioExist) return;
    g_audio_acc += kAudioSampleRate;
    s32 n = g_audio_acc / 60;   // ~533 stereo frames/field on average
    g_audio_acc %= 60;
    if (n > 0) audioCallbackWithMSound(n);  // NULL when not playing; drains when playing
}

// ---------------------------------------------------------------------------
// Audio mixer (MixAudio) + the device callback (audioCallbackWithMSound).
// ---------------------------------------------------------------------------
static void MixAudio(s16* destination, s16* /*unused*/, u32 sample);

static s16* audioCallbackWithMSound(s32 p1)
{
    if (ActivePlayer.open == FALSE || ActivePlayer.internalState != 2
        || ActivePlayer.audioExist == 0) {
        return (s16*)NULL;
    }
    BOOL enable = OSEnableInterrupts();
    SoundBufferIndex ^= 1;
    MixAudio(SoundBuffer[SoundBufferIndex], 0, p1);
    OSRestoreInterrupts(enable);
    return SoundBuffer[SoundBufferIndex];
}

static void MixAudio(s16* destination, s16* /*unused*/, u32 sample)
{
    if (ActivePlayer.open && ActivePlayer.internalState == 2
        && ActivePlayer.audioExist) {
        u32 sampleNum, requestSample = sample;
        s16* dst = destination;
        s32 i;
        s16* curPtr;
        s32 r_mix, l_mix;
        u16 attenuation;

        do {
            do {
                if (ActivePlayer.playAudioBuffer == NULL) {
                    if (!(ActivePlayer.playAudioBuffer =
                              (THPAudioBuffer*)PopDecodedAudioBuffer(0))) {
                        memset(dst, 0, requestSample * 4);
                        return;
                    }
                    ActivePlayer.curAudioNumber++;
                }
            } while ((sampleNum = ActivePlayer.playAudioBuffer->validSample) == 0);

            sampleNum = sampleNum >= requestSample ? requestSample : sampleNum;
            curPtr = ActivePlayer.playAudioBuffer->curPtr;

            for (i = 0; i < (s32)sampleNum; i++) {
                if (ActivePlayer.rampCount != 0) {
                    ActivePlayer.rampCount--;
                    ActivePlayer.curVolume += ActivePlayer.deltaVolume;
                } else {
                    ActivePlayer.curVolume = ActivePlayer.targetVolume;
                }
                attenuation = VolumeTable[(s32)ActivePlayer.curVolume];

                l_mix = attenuation * curPtr[0] >> 15;
                if (l_mix < -32768) l_mix = -32768;
                if (l_mix > 32767)  l_mix = 32767;
                dst[0] = (s16)l_mix;

                r_mix = attenuation * curPtr[1] >> 15;
                if (r_mix < -32768) r_mix = -32768;
                if (r_mix > 32767)  r_mix = 32767;
                dst[1] = (s16)r_mix;

                dst += 2; curPtr += 2;
            }

            requestSample -= sampleNum;
            ActivePlayer.playAudioBuffer->validSample -= sampleNum;
            ActivePlayer.playAudioBuffer->curPtr = curPtr;

            if (ActivePlayer.playAudioBuffer->validSample == 0) {
                PushFreeAudioBuffer(ActivePlayer.playAudioBuffer);
                ActivePlayer.playAudioBuffer = NULL;
            }
            if (requestSample == 0) break;
        } while (TRUE);
    } else {
        memset(destination, 0, sample * 4);
    }
}

// ---------------------------------------------------------------------------
// Used-texture-set recycle queue (THPPlayerDrawDone / PostDrawDone / Stop).
// ---------------------------------------------------------------------------
static void PushUsedTextureSet(OSMessage msg)
{
    OSSendMessage(&UsedTextureSetQueue, msg, OS_MESSAGE_NOBLOCK);
}
static OSMessage PopUsedTextureSet()
{
    OSMessage msg;
    if (OSReceiveMessage(&UsedTextureSetQueue, &msg, OS_MESSAGE_NOBLOCK) == 1)
        return msg;
    return NULL;
}

// ---------------------------------------------------------------------------
// PrepareReady handshake (read/decode threads signal Prepare to proceed).
// PrepareReady is declared with C++ linkage in THPPlayer.h (outside extern "C").
// ---------------------------------------------------------------------------
static void InitAllMessageQueue()
{
    int i;
    if (ActivePlayer.onMemory == FALSE)
        for (i = 0; i < THP_READ_BUFFER_COUNT; i++)
            PushFreeReadBuffer(&ActivePlayer.readBuffer[i]);

    for (i = 0; i < THP_TEXTURE_SET_COUNT; i++)
        PushFreeTextureSet(&ActivePlayer.textureSet[i]);

    if (ActivePlayer.audioExist)
        for (i = 0; i < THP_AUDIO_BUFFER_COUNT; i++)
            PushFreeAudioBuffer(&ActivePlayer.audioBuffer[i]);

    OSInitMessageQueue(&PrepareReadyQueue, &PrepareReadyMessage, 1);
}

static BOOL WaitUntilPrepare()
{
    OSMessage msg;
    OSReceiveMessage(&PrepareReadyQueue, &msg, OS_MESSAGE_BLOCK);
    return (BOOL)(intptr_t)msg ? TRUE : FALSE;
}

void PrepareReady(int msg)
{
    OSSendMessage(&PrepareReadyQueue, (OSMessage)(intptr_t)msg, OS_MESSAGE_BLOCK);
}

// ---------------------------------------------------------------------------
// Frame-timing predicates (PlayControl uses these).
// ---------------------------------------------------------------------------
static BOOL ProperTimingForStart()
{
    if (ActivePlayer.videoInfo.videoType & 1) {
        if (VIGetNextField() == 0) return TRUE;
    } else if (ActivePlayer.videoInfo.videoType & 2) {
        if (VIGetNextField() == 1) return TRUE;
    } else {
        return TRUE;
    }
    return FALSE;
}

static BOOL ProperTimingForGettingNextFrame()
{
    if (ActivePlayer.videoInfo.videoType & 1) {
        if (VIGetNextField() == 0) return TRUE;
    } else if (ActivePlayer.videoInfo.videoType & 2) {
        if (VIGetNextField() == 1) return TRUE;
    } else {
        s32 frameRate = (s32)(ActivePlayer.header.frameRate * 100.0f);
        if (VIGetTvFormat() == VI_TVMODE_PAL_INT)
            ActivePlayer.curCount = (s32)(ActivePlayer.retaceCount * frameRate / 5000);
        else
            ActivePlayer.curCount = (s32)(ActivePlayer.retaceCount * frameRate / 5994);

        if (ActivePlayer.prevCount != ActivePlayer.curCount) {
            ActivePlayer.prevCount = ActivePlayer.curCount;
            return TRUE;
        }
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// PlayControl — the VI post-retrace callback. Pops decoded frames, advances the
// frame counters, and (for an audio movie) detects end-of-movie -> state = 3.
// ---------------------------------------------------------------------------
static void PlayControl(u32 retraceCnt)
{
    THPTextureSet* decodedTexture;

    if (OldVIPostCallback != NULL)
        OldVIPostCallback(retraceCnt);

    decodedTexture = (THPTextureSet*)-1;
    if (ActivePlayer.open && ActivePlayer.state == 2) {
        // Pull this field's audio (advances curAudioNumber) — the AI-DMA stand-in.
        audio_tick();

        if (ActivePlayer.dvdError || ActivePlayer.videoError) {
            ActivePlayer.internalState = 5;
            ActivePlayer.state         = 5;
            if (std::getenv("SB_THP_DBG") || std::getenv("SB_MOVIE_DBG"))
                std::printf("[thp] END-OF-MOVIE: state->5 (error) dvd=%d video=%d\n",
                            ActivePlayer.dvdError, ActivePlayer.videoError);
            return;
        }

        if (std::getenv("SB_THP_DBG") && (ActivePlayer.retaceCount % 120) == 0)
            std::printf("[thp] play retrace=%lld internal=%d curAudio=%d curVideo=%d\n",
                        (long long)ActivePlayer.retaceCount, ActivePlayer.internalState,
                        ActivePlayer.curAudioNumber, ActivePlayer.curVideoNumber);

        ++ActivePlayer.retaceCount;

        if (ActivePlayer.retaceCount == 0) {
            if (ProperTimingForStart()) {
                if (ActivePlayer.audioExist) {
                    if (ActivePlayer.curVideoNumber - ActivePlayer.curAudioNumber <= 1) {
                        decodedTexture = (THPTextureSet*)PopDecodedTextureSet(0);
                        ActivePlayer.videoDecodeCount--;
                        ActivePlayer.curVideoNumber++;
                    } else {
                        ActivePlayer.internalState = 2;
                    }
                } else {
                    decodedTexture = (THPTextureSet*)PopDecodedTextureSet(0);
                }
            } else {
                ActivePlayer.retaceCount = -1;
            }
        } else {
            if (ActivePlayer.retaceCount == 1)
                ActivePlayer.internalState = 2;

            if (ProperTimingForGettingNextFrame()) {
                if (ActivePlayer.audioExist) {
                    if (ActivePlayer.curVideoNumber - ActivePlayer.curAudioNumber <= 1) {
                        decodedTexture = (THPTextureSet*)PopDecodedTextureSet(0);
                        ActivePlayer.videoDecodeCount--;
                        ActivePlayer.curVideoNumber++;
                    }
                } else {
                    decodedTexture = (THPTextureSet*)PopDecodedTextureSet(0);
                }
            }
        }

        if (decodedTexture != NULL && decodedTexture != (THPTextureSet*)-1) {
            if (ActivePlayer.dispTextureSet != NULL)
                PushUsedTextureSet(ActivePlayer.dispTextureSet);
            ActivePlayer.dispTextureSet = decodedTexture;
        }

        if ((ActivePlayer.playFlag & 1) == 0) {
            if (ActivePlayer.audioExist) {
                s32 audioFrame = ActivePlayer.curAudioNumber + ActivePlayer.initReadFrame;
                if (audioFrame == (s32)ActivePlayer.header.numFrames
                    && ActivePlayer.playAudioBuffer == NULL) {
                    ActivePlayer.internalState = 3;
                    ActivePlayer.state         = 3;
                    if (std::getenv("SB_THP_DBG") || std::getenv("SB_MOVIE_DBG"))
                        std::printf("[thp] END-OF-MOVIE: state->3 (audio) "
                                    "curAudioNumber=%d numFrames=%u retrace=%lld\n",
                                    ActivePlayer.curAudioNumber,
                                    ActivePlayer.header.numFrames,
                                    (long long)ActivePlayer.retaceCount);
                }
            } else {
                s32 curFrame;
                if (ActivePlayer.dispTextureSet != NULL)
                    curFrame = ActivePlayer.dispTextureSet->frameNumber
                               + ActivePlayer.initReadFrame;
                else
                    curFrame = ActivePlayer.initReadFrame - 1;
                if (curFrame == (s32)ActivePlayer.header.numFrames - 1
                    && decodedTexture == NULL) {
                    ActivePlayer.internalState = 3;
                    ActivePlayer.state         = 3;
                }
            }
        }
    }
}

// ===========================================================================
// extern "C" public API
// ===========================================================================
extern "C" {

BOOL THPPlayerInit()
{
    if (std::getenv("SB_MOVIE_DBG"))
        std::printf("[thp] THPPlayerInit (memset ActivePlayer; prev state=%d)\n",
                    (int)ActivePlayer.state);
    memset(&ActivePlayer, 0, sizeof(THPPlayer));
    OSInitMessageQueue(&UsedTextureSetQueue, UsedTextureSetMessage, 3);
    // GC: LCEnable (locked L1 cache for the IDCT scratch) + THPInit() + initAudio()
    // (JASDriver mix callback). The locked cache / DSP do not exist natively; the
    // audio callback is driven by audio_pump (started in Prepare).
    SoundBufferIndex = 0;
    memset(&SoundBuffer, 0, sizeof(SoundBuffer));
    Initialized = TRUE;
    return TRUE;
}

void THPPlayerQuit()
{
    Initialized = FALSE;
}

BOOL THPPlayerOpen(const char* fileName, BOOL onMemory)
{
    s32 offset, i;

    if (Initialized == FALSE) return FALSE;
    if (ActivePlayer.open)    return FALSE;

    memset(&ActivePlayer.videoInfo, 0, sizeof(THPVideoInfo));
    memset(&ActivePlayer.audioInfo, 0, sizeof(THPAudioInfo));

    if (DVDOpen((char*)fileName, &ActivePlayer.fileInfo) == FALSE)
        return FALSE;

    if (DVDReadPrio(&ActivePlayer.fileInfo, WorkBuffer, 64, 0, 2) < 0) {
        DVDClose(&ActivePlayer.fileInfo);
        return FALSE;
    }

    // BE: parse the THPHeader out of the disc bytes (decomp memcpy'd it native on GC).
    const u8* h = (const u8*)WorkBuffer;
    std::memcpy(ActivePlayer.header.magic, h, 4);
    ActivePlayer.header.version               = rd_be32(h + 4);
    ActivePlayer.header.bufsize               = rd_be32(h + 8);
    ActivePlayer.header.audioMaxSamples       = rd_be32(h + 12);
    ActivePlayer.header.frameRate             = rd_bef32(h + 16);
    ActivePlayer.header.numFrames             = rd_be32(h + 20);
    ActivePlayer.header.firstFrameSize        = rd_be32(h + 24);
    ActivePlayer.header.movieDataSize         = rd_be32(h + 28);
    ActivePlayer.header.compInfoDataOffsets   = rd_be32(h + 32);
    ActivePlayer.header.offsetDataOffsets     = rd_be32(h + 36);
    ActivePlayer.header.movieDataOffsets      = rd_be32(h + 40);
    ActivePlayer.header.finalFrameDataOffsets = rd_be32(h + 44);

    if (strncmp(ActivePlayer.header.magic, "THP", 3) != 0) {
        DVDClose(&ActivePlayer.fileInfo);
        return FALSE;
    }
    if (ActivePlayer.header.version != 0x11000) {
        DVDClose(&ActivePlayer.fileInfo);
        return FALSE;
    }

    offset = ActivePlayer.header.compInfoDataOffsets;
    if (DVDReadPrio(&ActivePlayer.fileInfo, WorkBuffer, 32, offset, 2) < 0) {
        DVDClose(&ActivePlayer.fileInfo);
        return FALSE;
    }
    ActivePlayer.compInfo.numComponents = rd_be32(WorkBuffer);
    std::memcpy(ActivePlayer.compInfo.frameComp, (const u8*)WorkBuffer + 4, 16);
    offset += (s32)sizeof(THPFrameCompInfo);  // 20
    ActivePlayer.audioExist = 0;

    for (i = 0; i < (s32)ActivePlayer.compInfo.numComponents; i++) {
        switch (ActivePlayer.compInfo.frameComp[i]) {
        case 0:
            if (DVDReadPrio(&ActivePlayer.fileInfo, WorkBuffer, 32, offset, 2) < 0) {
                DVDClose(&ActivePlayer.fileInfo);
                return FALSE;
            }
            ActivePlayer.videoInfo.xSize     = rd_be32((u8*)WorkBuffer + 0);
            ActivePlayer.videoInfo.ySize     = rd_be32((u8*)WorkBuffer + 4);
            ActivePlayer.videoInfo.videoType = rd_be32((u8*)WorkBuffer + 8);
            offset += (s32)sizeof(THPVideoInfo);  // 12
            break;
        case 1:
            if (DVDReadPrio(&ActivePlayer.fileInfo, WorkBuffer, 32, offset, 2) < 0) {
                DVDClose(&ActivePlayer.fileInfo);
                return FALSE;
            }
            ActivePlayer.audioInfo.sndChannels   = rd_be32((u8*)WorkBuffer + 0);
            ActivePlayer.audioInfo.sndFrequency  = rd_be32((u8*)WorkBuffer + 4);
            ActivePlayer.audioInfo.sndNumSamples = rd_be32((u8*)WorkBuffer + 8);
            ActivePlayer.audioInfo.sndNumTracks  = rd_be32((u8*)WorkBuffer + 12);
            offset += (s32)sizeof(THPAudioInfo);  // 16
            ActivePlayer.audioExist = 1;
            break;
        default:
            return FALSE;
        }
    }

    ActivePlayer.internalState = 0;
    ActivePlayer.state         = 0;
    ActivePlayer.playFlag      = 0;
    ActivePlayer.onMemory      = onMemory;
    ActivePlayer.open          = TRUE;
    ActivePlayer.curVolume     = 127.0f;
    ActivePlayer.targetVolume  = ActivePlayer.curVolume;
    ActivePlayer.rampCount     = 0;
    return TRUE;
}

BOOL THPPlayerClose()
{
    if (ActivePlayer.open && ActivePlayer.state == 0) {
        ActivePlayer.open = FALSE;
        DVDClose(&ActivePlayer.fileInfo);
        return TRUE;
    }
    return FALSE;
}

u32 THPPlayerCalcNeedMemory()
{
    if (ActivePlayer.open) {
        u32 size = ActivePlayer.onMemory
                       ? ALIGN_NEXT(ActivePlayer.header.movieDataSize, 32)
                       : ALIGN_NEXT(ActivePlayer.header.bufsize, 32) * 10;
        size += ALIGN_NEXT(ActivePlayer.videoInfo.xSize * ActivePlayer.videoInfo.ySize, 32) * 3;
        size += ALIGN_NEXT(ActivePlayer.videoInfo.xSize * ActivePlayer.videoInfo.ySize / 4, 32) * 3;
        size += ALIGN_NEXT(ActivePlayer.videoInfo.xSize * ActivePlayer.videoInfo.ySize / 4, 32) * 3;
        if (ActivePlayer.audioExist)
            size += ALIGN_NEXT(ActivePlayer.header.audioMaxSamples * 4, 32)
                    * THP_AUDIO_BUFFER_COUNT;
        return size + 0x1000;
    }
    return 0;
}

BOOL THPPlayerSetBuffer(u8* buffer)
{
    u32 i, ysize, uvsize;
    u8* ptr;

    if (ActivePlayer.open && ActivePlayer.state == 0) {
        ptr = buffer;
        if (ActivePlayer.onMemory) {
            ActivePlayer.movieData = buffer;
            ptr += ActivePlayer.header.movieDataSize;
        } else {
            for (i = 0; i < THP_READ_BUFFER_COUNT; i++) {
                ActivePlayer.readBuffer[i].ptr = ptr;
                ptr += ALIGN_NEXT(ActivePlayer.header.bufsize, 32);
            }
        }

        ysize  = ALIGN_NEXT(ActivePlayer.videoInfo.xSize * ActivePlayer.videoInfo.ySize, 32);
        uvsize = ALIGN_NEXT(ActivePlayer.videoInfo.xSize * ActivePlayer.videoInfo.ySize / 4, 32);

        for (i = 0; i < THP_TEXTURE_SET_COUNT; i++) {
            ActivePlayer.textureSet[i].ytexture = ptr; ptr += ysize;
            ActivePlayer.textureSet[i].utexture = ptr; ptr += uvsize;
            ActivePlayer.textureSet[i].vtexture = ptr; ptr += uvsize;
        }

        if (ActivePlayer.audioExist) {
            for (i = 0; i < THP_AUDIO_BUFFER_COUNT; i++) {
                ActivePlayer.audioBuffer[i].buffer      = (s16*)ptr;
                ActivePlayer.audioBuffer[i].curPtr      = (s16*)ptr;
                ActivePlayer.audioBuffer[i].validSample = 0;
                ptr += ALIGN_NEXT(ActivePlayer.header.audioMaxSamples * 4, 32);
            }
        }

        ActivePlayer.thpWork = ptr;
        return TRUE;
    }
    return FALSE;
}

BOOL THPPlayerPrepare(s32 frame, u8 flag, s32 audioTrack)
{
    u8* threadData;
    if (!(ActivePlayer.open && ActivePlayer.state == 0)) return FALSE;

    if (frame > 0) {
        if (ActivePlayer.header.offsetDataOffsets == 0) return FALSE;
        if (ActivePlayer.header.numFrames > (u32)frame) {
            if (DVDReadPrio(&ActivePlayer.fileInfo, WorkBuffer, 0x20,
                            ActivePlayer.header.offsetDataOffsets + (frame - 1) * 4, 2) < 0)
                return FALSE;
            ActivePlayer.initOffset    = ActivePlayer.header.movieDataOffsets
                                         + (s32)rd_be32((u8*)WorkBuffer + 0);
            ActivePlayer.initReadFrame = frame;
            ActivePlayer.initReadSize  = (s32)rd_be32((u8*)WorkBuffer + 4)
                                         - (s32)rd_be32((u8*)WorkBuffer + 0);
        } else {
            return FALSE;
        }
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

    if (ActivePlayer.onMemory) {
        if (DVDReadPrio(&ActivePlayer.fileInfo, ActivePlayer.movieData,
                        ActivePlayer.header.movieDataSize,
                        ActivePlayer.header.movieDataOffsets, 2) < 0)
            return FALSE;
        threadData = ActivePlayer.movieData + ActivePlayer.initOffset
                     - ActivePlayer.header.movieDataOffsets;
        CreateVideoDecodeThread(20, threadData);
        if (ActivePlayer.audioExist)
            CreateAudioDecodeThread(12, threadData);
    } else {
        CreateVideoDecodeThread(20, 0);
        if (ActivePlayer.audioExist)
            CreateAudioDecodeThread(12, NULL);
        CreateReadThread(8);
    }

    InitAllMessageQueue();
    VideoDecodeThreadStart();
    if (ActivePlayer.audioExist)
        AudioDecodeThreadStart();
    if (ActivePlayer.onMemory == 0)
        ReadThreadStart();

    if (!WaitUntilPrepare())
        return FALSE;

    ActivePlayer.state           = 1;
    ActivePlayer.internalState   = 0;
    ActivePlayer.dispTextureSet  = NULL;
    ActivePlayer.playAudioBuffer = NULL;
    ActivePlayer.curVideoNumber  = 0;
    ActivePlayer.curAudioNumber  = 0;

    OldVIPostCallback = VISetPostRetraceCallback(PlayControl);

    // Native: audio is pulled per-retrace from PlayControl (audio_tick); reset the
    // fractional-sample carry. On GC this is the registered JASDriver mix callback.
    audio_clock_reset();

    return TRUE;
}

BOOL THPPlayerPlay()
{
    if (ActivePlayer.open && (ActivePlayer.state == 1 || ActivePlayer.state == 4)) {
        ActivePlayer.state       = 2;
        ActivePlayer.prevCount   = 0;
        ActivePlayer.curCount    = 0;
        ActivePlayer.retaceCount = -1;
        return TRUE;
    }
    return FALSE;
}

void THPPlayerStop()
{
    if (std::getenv("SB_MOVIE_DBG"))
        std::printf("[thp] THPPlayerStop (state=%d open=%d)\n",
                    (int)ActivePlayer.state, (int)ActivePlayer.open);
    if (ActivePlayer.open && ActivePlayer.state != 0) {
        ActivePlayer.internalState = 0;
        ActivePlayer.state         = 0;
        VISetPostRetraceCallback(OldVIPostCallback);
        if (ActivePlayer.onMemory == 0) {
            ReadThreadCancel();
        }
        VideoDecodeThreadCancel();
        if (ActivePlayer.audioExist) {
            AudioDecodeThreadCancel();
        }
        while (PopUsedTextureSet() != NULL) { }

        ActivePlayer.curVolume  = ActivePlayer.targetVolume;
        ActivePlayer.rampCount  = 0;
        ActivePlayer.dvdError   = 0;
        ActivePlayer.videoError = 0;
    }
}

BOOL THPPlayerPause()
{
    if (ActivePlayer.open && ActivePlayer.state == 2) {
        ActivePlayer.internalState = 4;
        ActivePlayer.state         = 4;
        return TRUE;
    }
    return FALSE;
}

BOOL THPPlayerGetVideoInfo(THPVideoInfo* videoInfo)
{
    if (ActivePlayer.open) {
        memcpy(videoInfo, &ActivePlayer.videoInfo, sizeof(THPVideoInfo));
        return TRUE;
    }
    return FALSE;
}

BOOL THPPlayerGetAudioInfo(THPAudioInfo* audioInfo)
{
    if (ActivePlayer.open) {
        memcpy(audioInfo, &ActivePlayer.audioInfo, sizeof(THPAudioInfo));
        return TRUE;
    }
    return FALSE;
}

u32 THPPlayerGetTotalFrame()
{
    if (ActivePlayer.open) return ActivePlayer.header.numFrames;
    return 0;
}

s32 THPPlayerGetState() { return (s32)ActivePlayer.state; }

// THPPlayerDrawCurrentFrame — STAGE B: GX YUV->RGB textured quad. The decode side is
// stubbed this session (no real pixels), so there is no frame to present yet; return
// -1 (the decomp's dispTextureSet==NULL branch). When the codec + a present path land,
// this draws ActivePlayer.dispTextureSet.
s32 THPPlayerDrawCurrentFrame(GXRenderModeObj* /*rmode*/, u32 /*x*/, u32 /*y*/,
                              u32 /*polygonW*/, u32 /*polygonH*/)
{
    return -1;
}

void THPPlayerDrawDone()
{
    // GC: GXDrawDone() syncs the FIFO. No FIFO natively. Recycle used texture sets
    // back to the free pool (faithful), so the video pipeline can keep flowing.
    if (Initialized) {
        void* tex;
        while ((tex = PopUsedTextureSet()) != NULL)
            PushFreeTextureSet(tex);
    }
}

void THPPlayerPostDrawDone()
{
    if (Initialized) {
        void* msg;
        while ((msg = PopUsedTextureSet()) != NULL)
            PushFreeTextureSet(msg);
    }
}

BOOL THPPlayerSetVolume(s32 vol, s32 duration)
{
    if (ActivePlayer.open && ActivePlayer.audioExist) {
        // GC chose 32 or 48 from AIGetDSPSampleRate(); native audio is 48 kHz.
        const u32 numSamples = 48;
        if (vol > 127) vol = 127;
        if (vol < 0)   vol = 0;
        if (duration > 60000) duration = 60000;
        if (duration < 0)     duration = 0;

        BOOL interrupt = OSDisableInterrupts();
        ActivePlayer.targetVolume = (f32)vol;
        if (duration != 0) {
            ActivePlayer.rampCount   = numSamples * duration;
            ActivePlayer.deltaVolume = (ActivePlayer.targetVolume - ActivePlayer.curVolume)
                                       / (f32)ActivePlayer.rampCount;
        } else {
            ActivePlayer.rampCount = 0;
            ActivePlayer.curVolume = ActivePlayer.targetVolume;
        }
        OSRestoreInterrupts(interrupt);
        return TRUE;
    }
    return FALSE;
}

} // extern "C"
