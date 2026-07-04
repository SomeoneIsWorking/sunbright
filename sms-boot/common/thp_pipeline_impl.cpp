// thp_pipeline.cpp — native PC port of the THP decode pipeline worker threads.
//
// Ports, faithfully and combined into one translation unit (mirroring four decomp
// files that share ActivePlayer + the inter-queue accessors):
//   reference/sms/src/THPPlayer/THPRead.c          (the disc Reader thread)
//   reference/sms/src/THPPlayer/THPVideoDecode.c   (the VideoDecoder thread)
//   reference/sms/src/THPPlayer/THPAudioDecode.c   (the AudioDecoder thread)
//   reference/sms/src/dolphin/thp/THPAudio.c       (THPAudioDecode: ADPCM codec)
//
// THREADING: the GC pipeline is genuinely threaded (OSCreateThread + bounded
// OSMessageQueue back-pressure). The native OS seam (os_impl.cpp) backs OSThread
// with a real host std::thread and OSMessageQueue with a blocking FIFO, so this is
// a FAITHFUL port — the threads, queues, and back-pressure run as on hardware.
//
// BIG-ENDIAN: the THP frame containers come straight off the GameCube disc, so the
// in-band size/offset words are big-endian. The decomp consumed them as native ints
// (the GC CPU is big-endian); on the little-endian host they must be byte-swapped
// at the point of use. Marked `// BE` below.
//
// STAGE A scope (this session): the VIDEO codec (THPVideoDecode, the paired-single
// JPEG-like IDCT in dolphin/thp/THPDec.c) is NOT yet ported — it is a documented
// STAGE-B stub that returns success without filling pixels. The pipeline still
// streams + demuxes the REAL Entrance.thp and advances frame-for-frame, which is
// what unblocks the movie (end-of-movie here is AUDIO-gated; see PlayControl). The
// AUDIO codec (ADPCM) IS fully ported so curAudioNumber advances on real data.

#include <THPPlayer/THPPlayer.h>
#include <THPPlayer/THPRead.h>
#include <THPPlayer/THPVideoDecode.h>
#include <THPPlayer/THPAudioDecode.h>
#include <dolphin/thp.h>
#include <dolphin/os.h>
#include <dolphin/dvd.h>

#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// Endianness helpers (disc data is big-endian).
// ---------------------------------------------------------------------------
static inline u32 be32(const void* p) {
    const u8* b = (const u8*)p;
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}
static inline s16 be16s(const void* p) {
    const u8* b = (const u8*)p;
    return (s16)(((u16)b[0] << 8) | b[1]);
}

#define STACK_SIZE 4096

// ===========================================================================
// THPRead.c — the disc Reader thread + read-buffer queues.
// ===========================================================================
static OSMessageQueue FreeReadBufferQueue;
static OSMessageQueue ReadedBufferQueue;
static OSMessageQueue ReadedBufferQueue2;
static OSMessage FreeReadBufferMessage[THP_READ_BUFFER_COUNT];
static OSMessage ReadedBufferMessage[THP_READ_BUFFER_COUNT];
static OSMessage ReadedBufferMessage2[THP_READ_BUFFER_COUNT];

static OSThread ReadThread;
static u8 ReadThreadStack[STACK_SIZE];
static BOOL ReadThreadCreated;

static void* Reader(void* arg);

extern "C" BOOL CreateReadThread(OSPriority prio)
{
    if (OSCreateThread(&ReadThread, Reader, NULL, ReadThreadStack + STACK_SIZE,
                       STACK_SIZE, prio, 1)
        == FALSE)
        return FALSE;

    OSInitMessageQueue(&FreeReadBufferQueue, FreeReadBufferMessage,
                       THP_READ_BUFFER_COUNT);
    OSInitMessageQueue(&ReadedBufferQueue, ReadedBufferMessage,
                       THP_READ_BUFFER_COUNT);
    OSInitMessageQueue(&ReadedBufferQueue2, ReadedBufferMessage2,
                       THP_READ_BUFFER_COUNT);
    ReadThreadCreated = TRUE;
    return TRUE;
}

extern "C" void ReadThreadStart(void)
{
    if (ReadThreadCreated)
        OSResumeThread(&ReadThread);
}

extern "C" void ReadThreadCancel(void)
{
    if (ReadThreadCreated) {
        OSCancelThread(&ReadThread);
        ReadThreadCreated = FALSE;
    }
}

static void* Reader(void* /*arg*/)
{
    THPReadBuffer* buf;
    s32 curFrame;
    s32 status;
    s32 offset       = ActivePlayer.initOffset;
    s32 initReadSize = ActivePlayer.initReadSize;
    s32 frame        = 0;

    while (TRUE) {
        buf    = (THPReadBuffer*)PopFreeReadBuffer();
        status = DVDReadPrio(&ActivePlayer.fileInfo, buf->ptr, initReadSize,
                             offset, 2);
        if (status != initReadSize) {
            if (status == -1)
                ActivePlayer.dvdError = -1;
            if (frame == 0)
                PrepareReady(FALSE);

            OSSuspendThread(&ReadThread);
        }

        buf->frameNumber = frame;
        PushReadedBuffer(buf);
        offset += initReadSize;
        initReadSize = (s32)be32(buf->ptr);  // BE: first word of frame = next size

        curFrame = (frame + ActivePlayer.initReadFrame)
                   % ActivePlayer.header.numFrames;
        if (curFrame == (s32)ActivePlayer.header.numFrames - 1) {
            if ((ActivePlayer.playFlag & 1))
                offset = ActivePlayer.header.movieDataOffsets;
            else
                OSSuspendThread(&ReadThread);
        }

        frame++;
    }
}

extern "C" void* PopReadedBuffer(void)
{
    void* buffer;
    OSReceiveMessage(&ReadedBufferQueue, &buffer, OS_MESSAGE_BLOCK);
    return buffer;
}
extern "C" void* PushReadedBuffer(void* buffer)
{
    OSSendMessage(&ReadedBufferQueue, buffer, 1);
    return buffer;
}
extern "C" void* PopFreeReadBuffer(void)
{
    void* buffer;
    OSReceiveMessage(&FreeReadBufferQueue, &buffer, OS_MESSAGE_BLOCK);
    return buffer;
}
extern "C" void PushFreeReadBuffer(void* buffer)
{
    OSSendMessage(&FreeReadBufferQueue, buffer, 1);
}
extern "C" void* PopReadedBuffer2(void)
{
    void* buffer;
    OSReceiveMessage(&ReadedBufferQueue2, &buffer, OS_MESSAGE_BLOCK);
    return buffer;
}
extern "C" void PushReadedBuffer2(void* buffer)
{
    OSSendMessage(&ReadedBufferQueue2, buffer, 1);
}

// ===========================================================================
// dolphin/thp/THPAudio.c — THPAudioDecode (ADPCM), faithfully ported.
// The on-disc THPAudioRecordHeader is big-endian; decode into a swapped local.
// ===========================================================================
static s32 __THPAudioGetNewSample(THPAudioDecodeInfo* info)
{
    s32 sample;
    if (!(info->offsetNibbles & 0x0f)) {
        info->predictor = (u8)((*(info->encodeData) & 0x70) >> 4);
        info->scale     = (u8)((*(info->encodeData) & 0xF));
        info->encodeData++;
        info->offsetNibbles += 2;
    }
    if (info->offsetNibbles & 0x1) {
        sample = (s32)((*(info->encodeData) & 0xF) << 28) >> 28;
        info->encodeData++;
    } else {
        sample = (s32)((*(info->encodeData) & 0xF0) << 24) >> 28;
    }
    info->offsetNibbles++;
    return sample;
}

static void __THPAudioInitialize(THPAudioDecodeInfo* info, u8* ptr)
{
    info->encodeData    = ptr;
    info->offsetNibbles = 2;
    info->predictor     = (u8)((*(info->encodeData) & 0x70) >> 4);
    info->scale         = (u8)((*(info->encodeData) & 0xF));
    info->encodeData++;
}

extern "C" u32 THPAudioDecode(s16* audioBuffer, u8* audioFrame, s32 flag)
{
    THPAudioRecordHeader hdr;       // host-order copy of the BE on-disc header
    THPAudioDecodeInfo decInfo;
    u8 *left, *right;
    s16 *decLeftPtr, *decRightPtr;
    s16 yn1, yn2;
    s32 i, step, sample;
    s64 yn;

    if (audioBuffer == NULL || audioFrame == NULL)
        return 0;

    // BE: byte-swap the record header fields out of the disc bytes.
    const u8* h = audioFrame;
    hdr.offsetNextChannel = be32(h + 0);
    hdr.sampleSize        = be32(h + 4);
    for (i = 0; i < 8; i++) {
        hdr.lCoef[i][0] = be16s(h + 8 + (i * 2 + 0) * 2);
        hdr.lCoef[i][1] = be16s(h + 8 + (i * 2 + 1) * 2);
    }
    for (i = 0; i < 8; i++) {
        hdr.rCoef[i][0] = be16s(h + 8 + 32 + (i * 2 + 0) * 2);
        hdr.rCoef[i][1] = be16s(h + 8 + 32 + (i * 2 + 1) * 2);
    }
    hdr.lYn1 = be16s(h + 8 + 64 + 0);
    hdr.lYn2 = be16s(h + 8 + 64 + 2);
    hdr.rYn1 = be16s(h + 8 + 64 + 4);
    hdr.rYn2 = be16s(h + 8 + 64 + 6);

    left  = audioFrame + sizeof(THPAudioRecordHeader);
    right = left + hdr.offsetNextChannel;

    if (flag == 1) {
        decRightPtr = audioBuffer;
        decLeftPtr  = audioBuffer + hdr.sampleSize;
        step        = 1;
    } else {
        decRightPtr = audioBuffer;
        decLeftPtr  = audioBuffer + 1;
        step        = 2;
    }

    if (hdr.offsetNextChannel == 0) {
        __THPAudioInitialize(&decInfo, left);
        yn1 = hdr.lYn1; yn2 = hdr.lYn2;
        for (i = 0; i < (s32)hdr.sampleSize; i++) {
            sample = __THPAudioGetNewSample(&decInfo);
            yn  = (s64)hdr.lCoef[decInfo.predictor][1] * yn2;
            yn += (s64)hdr.lCoef[decInfo.predictor][0] * yn1;
            yn += ((s64)(sample << decInfo.scale)) << 11;
            yn <<= 5;
            if ((u16)(yn & 0xffff) > 0x8000) yn += 0x10000;
            else if ((u16)(yn & 0xffff) == 0x8000) { if (yn & 0x10000) yn += 0x10000; }
            if (yn > 2147483647LL) yn = 2147483647LL;
            if (yn < -2147483648LL) yn = -2147483648LL;
            *decLeftPtr  = (s16)(yn >> 16); decLeftPtr  += step;
            *decRightPtr = (s16)(yn >> 16); decRightPtr += step;
            yn2 = yn1; yn1 = (s16)(yn >> 16);
        }
    } else {
        __THPAudioInitialize(&decInfo, left);
        yn1 = hdr.lYn1; yn2 = hdr.lYn2;
        for (i = 0; i < (s32)hdr.sampleSize; i++) {
            sample = __THPAudioGetNewSample(&decInfo);
            yn  = (s64)hdr.lCoef[decInfo.predictor][1] * yn2;
            yn += (s64)hdr.lCoef[decInfo.predictor][0] * yn1;
            yn += ((s64)(sample << decInfo.scale)) << 11;
            yn <<= 5;
            if ((u16)(yn & 0xffff) > 0x8000) yn += 0x10000;
            else if ((u16)(yn & 0xffff) == 0x8000) { if (yn & 0x10000) yn += 0x10000; }
            if (yn > 2147483647LL) yn = 2147483647LL;
            if (yn < -2147483648LL) yn = -2147483648LL;
            *decLeftPtr = (s16)(yn >> 16); decLeftPtr += step;
            yn2 = yn1; yn1 = (s16)(yn >> 16);
        }
        __THPAudioInitialize(&decInfo, right);
        yn1 = hdr.rYn1; yn2 = hdr.rYn2;
        for (i = 0; i < (s32)hdr.sampleSize; i++) {
            sample = __THPAudioGetNewSample(&decInfo);
            yn  = (s64)hdr.rCoef[decInfo.predictor][1] * yn2;
            yn += (s64)hdr.rCoef[decInfo.predictor][0] * yn1;
            yn += ((s64)(sample << decInfo.scale)) << 11;
            yn <<= 5;
            if ((u16)(yn & 0xffff) > 0x8000) yn += 0x10000;
            else if ((u16)(yn & 0xffff) == 0x8000) { if (yn & 0x10000) yn += 0x10000; }
            if (yn > 2147483647LL) yn = 2147483647LL;
            if (yn < -2147483648LL) yn = -2147483648LL;
            *decRightPtr = (s16)(yn >> 16); decRightPtr += step;
            yn2 = yn1; yn1 = (s16)(yn >> 16);
        }
    }

    return hdr.sampleSize;
}

// ===========================================================================
// THPVideoDecode.c (codec call site) — dolphin/thp/THPDec.c — STAGE-A STUB.
//
// The real codec is a paired-single (Gekko-asm) baseline-JPEG-like IDCT that fills
// the Y/U/V tile textures. That port is STAGE B (visible frames). For STAGE A the
// pipeline only needs a texture set produced per frame at the correct cadence — the
// end-of-movie gate for Entrance.thp is AUDIO-driven (see PlayControl), so video
// pixel content is not on the unblock critical path. Returns 0 (success), no error,
// so VideoDecode pushes the (black) texture set and frame counting proceeds exactly
// as it would with real pixels. NOT a faked state advance: the demux/stream/cadence
// are all real disc data — only the inner pixel IDCT is deferred.
extern "C" s32 THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV,
                              void* work)
{
    if (!file) return 25;
    if (!tileY || !tileU || !tileV) return 27;
    if (!work) return 26;
    return 0;  // STAGE B: real IDCT fills tileY/U/V here.
}

// ===========================================================================
// THPVideoDecode.c — the VideoDecoder thread + texture-set queues.
// ===========================================================================
static OSThread VideoDecodeThread;
static u8 VideoDecodeThreadStack[STACK_SIZE];
static OSMessageQueue FreeTextureSetQueue;
static OSMessageQueue DecodedTextureSetQueue;
static OSMessage FreeTextureSetMessage[THP_AUDIO_BUFFER_COUNT];
static OSMessage DecodedTextureSetMessage[THP_AUDIO_BUFFER_COUNT];
static BOOL VideoDecodeThreadCreated;
static BOOL VFirst;

static void* VideoDecoderForOnMemory(void* arg);
static void* VideoDecoder(void* arg);
static void  VideoDecode(THPReadBuffer* readBuffer);

extern "C" BOOL CreateVideoDecodeThread(OSPriority prio, void* arg)
{
    BOOL res;
    if (arg) {
        res = OSCreateThread(&VideoDecodeThread, VideoDecoderForOnMemory, arg,
                             VideoDecodeThreadStack + STACK_SIZE, STACK_SIZE,
                             prio, 1);
    } else {
        res = OSCreateThread(&VideoDecodeThread, VideoDecoder, NULL,
                             VideoDecodeThreadStack + STACK_SIZE, STACK_SIZE,
                             prio, 1);
    }
    if (res == FALSE)
        return FALSE;

    OSInitMessageQueue(&FreeTextureSetQueue, FreeTextureSetMessage,
                       THP_AUDIO_BUFFER_COUNT);
    OSInitMessageQueue(&DecodedTextureSetQueue, DecodedTextureSetMessage,
                       THP_AUDIO_BUFFER_COUNT);
    VideoDecodeThreadCreated = TRUE;
    VFirst                   = TRUE;
    return TRUE;
}

extern "C" void VideoDecodeThreadStart(void)
{
    if (VideoDecodeThreadCreated)
        OSResumeThread(&VideoDecodeThread);
}

extern "C" void VideoDecodeThreadCancel(void)
{
    if (VideoDecodeThreadCreated) {
        OSCancelThread(&VideoDecodeThread);
        VideoDecodeThreadCreated = FALSE;
    }
}

static void* VideoDecoder(void* /*arg*/)
{
    THPReadBuffer* thpBuffer;
    while (TRUE) {
        if (ActivePlayer.audioExist) {
            for (; ActivePlayer.videoDecodeCount < 0;) {
                thpBuffer = (THPReadBuffer*)PopReadedBuffer2();
                s32 remaining
                    = ((thpBuffer->frameNumber + ActivePlayer.initReadFrame)
                       % ActivePlayer.header.numFrames);
                if (remaining == (s32)(ActivePlayer.header.numFrames - 1)
                    && (ActivePlayer.playFlag & 1) == 0)
                    VideoDecode(thpBuffer);

                PushFreeReadBuffer(thpBuffer);
                BOOL enable = OSDisableInterrupts();
                ActivePlayer.videoDecodeCount++;
                OSRestoreInterrupts(enable);
            }
        }

        if (ActivePlayer.audioExist)
            thpBuffer = (THPReadBuffer*)PopReadedBuffer2();
        else
            thpBuffer = (THPReadBuffer*)PopReadedBuffer();

        VideoDecode(thpBuffer);
        PushFreeReadBuffer(thpBuffer);
    }
}

static void* VideoDecoderForOnMemory(void* arg)
{
    THPReadBuffer readBuffer;
    s32 readSize, frame, remaining;

    readSize       = ActivePlayer.initReadSize;
    frame          = 0;
    readBuffer.ptr = (u8*)arg;

    while (TRUE) {
        if (ActivePlayer.audioExist) {
            while (ActivePlayer.videoDecodeCount < 0) {
                BOOL enable = OSDisableInterrupts();
                ActivePlayer.videoDecodeCount++;
                OSRestoreInterrupts(enable);
                remaining = (frame + ActivePlayer.initReadFrame)
                            % ActivePlayer.header.numFrames;
                if (remaining == (s32)ActivePlayer.header.numFrames - 1) {
                    if ((ActivePlayer.playFlag & 1) == 0)
                        break;
                    readSize       = (s32)be32(readBuffer.ptr);  // BE
                    readBuffer.ptr = ActivePlayer.movieData;
                } else {
                    s32 size = (s32)be32(readBuffer.ptr);        // BE
                    readBuffer.ptr += readSize;
                    readSize = size;
                }
                frame++;
            }
        }

        readBuffer.frameNumber = frame;
        VideoDecode(&readBuffer);

        remaining = (frame + ActivePlayer.initReadFrame)
                    % ActivePlayer.header.numFrames;
        if (remaining == (s32)ActivePlayer.header.numFrames - 1) {
            if ((ActivePlayer.playFlag & 1)) {
                readSize       = (s32)be32(readBuffer.ptr);      // BE
                readBuffer.ptr = ActivePlayer.movieData;
            } else {
                OSSuspendThread(&VideoDecodeThread);
            }
        } else {
            s32 size = (s32)be32(readBuffer.ptr);                // BE
            readBuffer.ptr += readSize;
            readSize = size;
        }
        frame++;
    }
}

static void VideoDecode(THPReadBuffer* readBuffer)
{
    THPTextureSet* textureSet;
    s32 i;
    u8* tileOffsets;   // points at the BE component-size table (ptr+8)
    u8* tile;

    tileOffsets = readBuffer->ptr + 8;
    tile        = &readBuffer->ptr[ActivePlayer.compInfo.numComponents * 4] + 8;
    textureSet  = (THPTextureSet*)PopFreeTextureSet();

    for (i = 0; i < (s32)ActivePlayer.compInfo.numComponents; i++) {
        switch (ActivePlayer.compInfo.frameComp[i]) {
        case 0: {
            if ((ActivePlayer.videoError = THPVideoDecode(
                     tile, textureSet->ytexture, textureSet->utexture,
                     textureSet->vtexture, ActivePlayer.thpWork))) {
                if (VFirst) { PrepareReady(FALSE); VFirst = FALSE; }
                OSSuspendThread(&VideoDecodeThread);
            }
            textureSet->frameNumber = readBuffer->frameNumber;
            PushDecodedTextureSet(textureSet);
            BOOL enable = OSDisableInterrupts();
            ActivePlayer.videoDecodeCount++;
            OSRestoreInterrupts(enable);
        }
        }
        tile += be32(tileOffsets);  // BE: component size
        tileOffsets += 4;
    }

    if (VFirst) { PrepareReady(TRUE); VFirst = FALSE; }
}

extern "C" void* PopFreeTextureSet(void)
{
    void* tex;
    OSReceiveMessage(&FreeTextureSetQueue, &tex, 1);
    return tex;
}
extern "C" void PushFreeTextureSet(void* tex)
{
    OSSendMessage(&FreeTextureSetQueue, tex, 0);
}
extern "C" void* PopDecodedTextureSet(s32 flags)
{
    void* tex;
    if (OSReceiveMessage(&DecodedTextureSetQueue, &tex, flags) == TRUE)
        return tex;
    return NULL;
}
extern "C" void PushDecodedTextureSet(void* tex)
{
    OSSendMessage(&DecodedTextureSetQueue, tex, OS_MESSAGE_BLOCK);
}

// ===========================================================================
// THPAudioDecode.c — the AudioDecoder thread + audio-buffer queues.
// ===========================================================================
static OSThread AudioDecodeThread;
static u8 AudioDecodeThreadStack[STACK_SIZE];
static OSMessageQueue FreeAudioBufferQueue;
static OSMessageQueue DecodedAudioBufferQueue;
static OSMessage FreeAudioBufferMessage[THP_AUDIO_BUFFER_COUNT];
static OSMessage DecodedAudioBufferMessage[THP_AUDIO_BUFFER_COUNT];
static BOOL AudioDecodeThreadCreated;

static void* AudioDecoderForOnMemory(void* arg);
static void* AudioDecoder(void* arg);
static void  AudioDecode(THPReadBuffer* readBuffer);

extern "C" BOOL CreateAudioDecodeThread(OSPriority prio, void* arg)
{
    BOOL res;
    if (arg) {
        res = OSCreateThread(&AudioDecodeThread, AudioDecoderForOnMemory, arg,
                             AudioDecodeThreadStack + STACK_SIZE, STACK_SIZE,
                             prio, 1);
    } else {
        res = OSCreateThread(&AudioDecodeThread, AudioDecoder, NULL,
                             AudioDecodeThreadStack + STACK_SIZE, STACK_SIZE,
                             prio, 1);
    }
    if (res == FALSE)
        return FALSE;

    OSInitMessageQueue(&FreeAudioBufferQueue, FreeAudioBufferMessage,
                       THP_AUDIO_BUFFER_COUNT);
    OSInitMessageQueue(&DecodedAudioBufferQueue, DecodedAudioBufferMessage,
                       THP_AUDIO_BUFFER_COUNT);
    AudioDecodeThreadCreated = TRUE;
    return TRUE;
}

extern "C" void AudioDecodeThreadStart()
{
    if (AudioDecodeThreadCreated)
        OSResumeThread(&AudioDecodeThread);
}

extern "C" void AudioDecodeThreadCancel()
{
    if (AudioDecodeThreadCreated) {
        OSCancelThread(&AudioDecodeThread);
        AudioDecodeThreadCreated = FALSE;
    }
}

static void* AudioDecoder(void* /*arg*/)
{
    THPReadBuffer* buf;
    while (TRUE) {
        buf = (THPReadBuffer*)PopReadedBuffer();
        AudioDecode(buf);
        PushReadedBuffer2(buf);
    }
}

static void* AudioDecoderForOnMemory(void* arg)
{
    s32 frame    = 0;
    s32 readSize = ActivePlayer.initReadSize;
    THPReadBuffer readBuffer;
    readBuffer.ptr = (u8*)arg;

    while (TRUE) {
        readBuffer.frameNumber = frame;
        AudioDecode(&readBuffer);

        s32 remaining = (frame + ActivePlayer.initReadFrame)
                        % ActivePlayer.header.numFrames;
        if (remaining == (s32)ActivePlayer.header.numFrames - 1) {
            if ((ActivePlayer.playFlag & 1)) {
                readSize       = (s32)be32(readBuffer.ptr);   // BE
                readBuffer.ptr = ActivePlayer.movieData;
            } else {
                OSSuspendThread(&AudioDecodeThread);
            }
        } else {
            s32 size = (s32)be32(readBuffer.ptr);             // BE
            readBuffer.ptr += readSize;
            readSize = size;
        }
        frame++;
    }
}

static void AudioDecode(THPReadBuffer* readBuffer)
{
    THPAudioBuffer* audioBuf;
    s32 i;
    u8* offsets;     // BE component-size table at ptr+8
    u8* audioData;

    offsets   = readBuffer->ptr + 8;
    audioData = &readBuffer->ptr[ActivePlayer.compInfo.numComponents * 4] + 8;
    audioBuf  = (THPAudioBuffer*)PopFreeAudioBuffer();

    for (i = 0; i < (s32)ActivePlayer.compInfo.numComponents; i++) {
        switch (ActivePlayer.compInfo.frameComp[i]) {
        case 1: {
            audioBuf->validSample = THPAudioDecode(
                audioBuf->buffer,
                (audioData + be32(offsets) * ActivePlayer.curAudioTrack), 0);
            audioBuf->curPtr = audioBuf->buffer;
            PushDecodedAudioBuffer(audioBuf);
            return;
        }
        }
        audioData += be32(offsets);  // BE
        offsets += 4;
    }
}

extern "C" void* PopFreeAudioBuffer()
{
    void* buf;
    OSReceiveMessage(&FreeAudioBufferQueue, &buf, 1);
    return buf;
}
extern "C" void PushFreeAudioBuffer(void* buf)
{
    OSSendMessage(&FreeAudioBufferQueue, buf, OS_MESSAGE_NOBLOCK);
}
extern "C" void* PopDecodedAudioBuffer(s32 flags)
{
    void* buf;
    if (OSReceiveMessage(&DecodedAudioBufferQueue, &buf, flags) == 1)
        return buf;
    return NULL;
}
extern "C" void PushDecodedAudioBuffer(void* buf)
{
    OSSendMessage(&DecodedAudioBufferQueue, buf, 1);
}
