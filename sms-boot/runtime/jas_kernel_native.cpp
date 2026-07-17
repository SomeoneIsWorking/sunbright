// jas_kernel_native.cpp — audio arc MILESTONE 1: run the decomp's own JASystem
// KERNEL synchronously on the game thread (see docs/audio_native_mixer_plan.md).
//
// On real hardware, JASystem::AudioThread::audioproc() (JASAudioThread.cpp) is a
// dedicated OSThread that: Kernel::init()s once, boots the DSP ucode, then loops
// forever servicing two interrupt sources (an AI-DMA "buffer consumed" callback
// that calls Kernel::updateDac(), and a DSP mailbox callback that drives
// DSPBuf::updateDSP()/finishDSPFrame()). Under SMS_NATIVE_PLATFORM audioproc()
// returns nullptr immediately (no thread body ever runs) — see the #ifdef at the
// top of that function — so Kernel::init() and Kernel::updateDac() never execute
// and JAS is completely inert. This file replaces the missing thread body with a
// synchronous per-video-frame pump: same calls, same order, called inline from
// sb_audio_frame() instead of from an interrupt-driven OS thread.
//
// Kernel::init() is exactly what audioproc() would have called first (JASAiCtrl.cpp):
// resetCallback / initSystem (allocates the 3 dac[] ping-pong buffers + AIInit/
// AIInitDMA, both no-op host stubs) / portCmdInit / Dvd::init / Calc::initSinfT.
// We deliberately do NOT call DspBoot() or Driver::init() here — Driver::init() is
// already invoked once, synchronously, by JASystem::AudioThread::start() under
// SMS_NATIVE_PLATFORM (JASAudioThread.cpp start(), the `else` branch of `if (flags
// & 2)`) because TDSPChannel/ChGlobal/DSPBuf allocation has to happen before any
// BGM/SE can be queued, independent of whether the DAC pump itself ever runs.
// Calling it again here would double-allocate dsp_buf[]/the channel pool. DspBoot
// is the literal GC-DSP-ucode mailbox boot handshake (osdsp_task.c, EXCLUDEd from
// this build, CMakeLists.txt) — there is no DSP to boot; AISetDSPSampleRate /
// AIRegisterDMACallback / AIStartDMA are already no-op host stubs
// (sms-boot/runtime/sdk_stubs.cpp) for the same reason.
//
// Gate: Kernel::init() touches the JAS system-DRAM heap (JASDram, set up by
// Kernel::sysDramSetup() inside AudioThread::start()) via allocFromSysDram(), so it
// must not run before that heap exists. AudioThread::start() is called from deep
// inside the MSound constructor (MSound.cpp: initDriver() runs before MSGMSound is
// assigned near the end of the ctor); Application.cpp assigns the global gpMSound
// only once `new MSound(...)` fully returns. So `SMSGetMSound() != nullptr` is
// exactly "AudioThread::start() has completed" and is the gate used below.
//
// Cadence math (see docs/audio_native_mixer_plan.md milestone 1): each
// Kernel::updateDac() call runs Kernel::vframeWork() (JASAiCtrl.cpp — the
// vframeWorkRunning/lastRspMadep bookkeeping causes vframeWork() to fire on every
// updateDac() call once primed) which produces exactly one full dac[] buffer:
// getFrameSamples() (JASRate.cpp: gFrameSamples = 0x230 = 560) STEREO FRAMES,
// i.e. getDacSize() (0x460 = 1120) s16 words. The real AI hardware plays that
// buffer at gDacRate = 32028.5 Hz (JASRate.cpp), so one dac[] buffer is
// 560 / 32028.5 s =~ 17.48 ms of audio — NOT an integer multiple of a 60 Hz video
// field (16.68 ms @ NTSC 60000/1001, or 16.67 ms @ a flat 60 Hz); on real hardware
// the two clocks are independent and simply drift/interleave via the AI FIFO depth
// (this is normal GC/Wii audio behavior, not a porting gap). We reproduce that with
// a sample-accumulator: each video frame we owe (nominal 32000 Hz / 60 Hz =~
// 533.33) samples; we call updateDac() (producing 560 samples each) until the
// accumulated debt is paid off. That yields ~0.952 calls/frame on average (mostly
// 1 call, occasionally 0), matching the ratio of the real constants instead of a
// hand-picked one-call-per-frame cadence.
//
// DsyncFrame2 (dspproc.h/dsptask.h) is the actual GC-DSP-ucode mailbox call that
// hands a subframe count + L/R buffer addresses to the DSP for rendering (the
// Zelda-ucode voice mixer — see docs/audio_native_mixer_plan.md "key identification").
// That voice renderer is MILESTONE 2, not this one. Milestone 1's DsyncFrame2 is a
// documented LOUD seam: it silences the two output buffers (so nothing garbage
// reaches aurora) and prints a one-time [STUB-CALLED] notice, proving every layer
// above it (BMS parsing, channel allocation, VPB fill, DSPBuf's triple-buffer
// pipeline) really executed up to the point where the actual sample synthesis
// would happen. DSPBuf::process() (JASDSPBuf.cpp) reaches DsyncFrame2 entirely
// through Kernel::updateDac() -> vframeWork() -> DSPBuf::mixDSP() -> process()'s
// own idle-kick ("if (dspstatus == 0) finishDSPFrame()") — no interrupt/thread is
// needed, it is reachable synchronously exactly like the rest of this pipeline.
//
// Getting the rendered samples out: updateDac() already ends with
// `if (dacCallbackFunc) dacCallbackFunc(lastRspMadep, getDacSize() / 2)` — the
// call site for a per-buffer host hook was fully wired, but
// Kernel::registerDacCallback() (JASAiCtrl.cpp) was an EMPTY decomp stub (no
// caller anywhere in SMS ever registered one on real hardware — the mechanism
// wires the field/null-check/call but nothing sets it, a decomp gap, not
// documented retail behavior). Filled it in faithfully (see JASAiCtrl.cpp) so we
// can register a real native sink here: lastRspMadep is the just-rendered dac[]
// buffer (getDacSize()/2 == getFrameSamples() == 560 interleaved S16 stereo
// FRAMES, per Calc::imixcopy in JASAiCtrl.cpp::vframeWork), which is exactly the
// (samples, num_frames) shape aurora_audio_push() wants.

#include <JSystem/JAudio/JASystem/JASAiCtrl.hpp>
#include <JSystem/JAudio/JASystem/JASDSPBuf.hpp>
#include <JSystem/JAudio/JASystem/JASDSPInterface.hpp>
#include <JSystem/JAudio/JASystem/JASRate.hpp>
#include <JSystem/dspproc.h>
#include <JSystem/dsptask.h>
#include <MSound/MSound.hpp>
#include <dolphin/types.h>

#include <aurora/audio.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Audio M2 accessor (JASDSPChannel.cpp): VPB for live DSP voice i, else null.
extern "C" JASystem::DSPInterface::DSPBuffer* sb_jas_dspch_vpb(int i);

namespace {

bool g_headless = false;
bool g_inited   = false;
bool g_audioOpen = false;

// ── Audio M2: native DSP voice renderer (Zelda-ucode per-voice mix) ────────────
// Replaces the milestone-1 silence seam. RE'd from the decomp + the proven
// recomp-era decoder (scratch/audio_ref/native_jas_recomp_era.cpp) — see
// docs/audio_native_mixer_plan.md "M2 VPB field map"/"pitch scale". bufL/bufR are
// SEPARATE live host L/R buffers (getFrameSamples() mono s16 each);
// vframeWork/imixcopy interleaves them into the stereo dac[] downstream.

// AFC (ADPCM) coefficient table — proven bit-exact (native_jas_recomp_era.cpp).
static const int kAfcCoef[16][2] = {
    {0,0},{2048,0},{0,2048},{1024,1024},{4096,-2048},{3584,-1536},{3072,-1024},{4608,-2560},
    {4200,-2248},{4800,-2300},{5120,-3072},{2048,-2048},{1024,-1024},{-1024,1024},{-1024,0},{-2048,0},
};
static inline u16 sb_be16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }

// Decode a whole AFC stream (big-endian .aw data) to s16 PCM. hq: 9B/16smp blocks.
static void sb_afc_decode(const u8* d, u32 len, bool hq, std::vector<s16>& out)
{
    int yn1 = 0, yn2 = 0;
    const u32 bs = hq ? 9 : 5;
    out.reserve(out.size() + (len / bs) * 16);
    for (u32 b = 0; b + bs <= len; b += bs) {
        const u8* blk = d + b;
        const int delta = 1 << (blk[0] >> 4);
        const int c0 = kAfcCoef[blk[0] & 0xF][0], c1 = kAfcCoef[blk[0] & 0xF][1];
        for (int i = 0; i < 16; i++) {
            int nib;
            if (hq) { nib = (blk[1 + i / 2] >> (i % 2 ? 0 : 4)) & 0xF; if (nib >= 8) nib -= 16; nib <<= 11; }
            else    { nib = (blk[1 + i / 4] >> (6 - 2 * (i % 4))) & 3; if (nib >= 2) nib -= 4; nib <<= 13; }
            int s = (delta * nib + yn1 * c0 + yn2 * c1) >> 11;
            if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
            yn2 = yn1; yn1 = s;
            out.push_back((s16)s);
        }
    }
}

// Host-side per-voice playback state (the ucode keeps this in the VPB; on the host
// the renderer replaces the ucode, so we persist it keyed by DSPCH index).
struct HostVoice {
    const void*       waveBase = nullptr; // VPB unk118 at last decode (re-trigger detect)
    std::vector<s16>  pcm;                // decoded wave cache
    double            cursor   = 0.0;     // fractional sample position
    bool              playing  = false;
};
static HostVoice g_hostVoice[64];

// Decode the voice's wave (VPB) into its PCM cache. Format from the VPB:
//   unk64==16 -> AFC (unk100==9 hq / ==5 lq);  unk64==1 -> PCM (unk100==16 / ==8 bits).
// unk11C = full length in BYTES (setWaveInfo: unk11C = Wave_.unk1C). Wave data is BE.
static void sb_decode_voice(JASystem::DSPInterface::DSPBuffer* vpb, HostVoice& hv)
{
    hv.pcm.clear();
    const u8* data = reinterpret_cast<const u8*>(vpb->unk118);
    const u32 lenBytes = vpb->unk11C;
    if (data == nullptr || lenBytes == 0)
        return;
    if (vpb->unk64 == 16) {
        sb_afc_decode(data, lenBytes, vpb->unk100 == 9, hv.pcm);
    } else if (vpb->unk64 == 1) {
        if (vpb->unk100 == 16) {
            hv.pcm.resize(lenBytes / 2);
            for (u32 i = 0; i < lenBytes / 2; i++) hv.pcm[i] = (s16)sb_be16(data + i * 2);
        } else if (vpb->unk100 == 8) {
            hv.pcm.resize(lenBytes);
            for (u32 i = 0; i < lenBytes; i++) hv.pcm[i] = (s16)((s8)data[i] << 8);
        }
    }
}

void dsyncFrame2Native(u32 /*subframes*/, uintptr_t bufL, uintptr_t bufR)
{
    static bool announced = false;
    if (!announced) {
        std::fprintf(stderr, "[audio] DsyncFrame2 native voice renderer active (M2 v1)\n");
        announced = true;
    }
    const u32 n = JASystem::Kernel::getFrameSamples();
    s16* L = reinterpret_cast<s16*>(bufL);
    s16* R = reinterpret_cast<s16*>(bufR);

    // Mix in 32-bit headroom to avoid mid-sum clipping, clamp at the end.
    static std::vector<s32> accL, accR;
    accL.assign(n, 0);
    accR.assign(n, 0);

    static int s_dbg = -1;
    if (s_dbg < 0) s_dbg = std::getenv("SB_DBG_AUDIO") ? 1 : 0;
    static long s_call = 0;
    static int s_maxLive = -1;
    const bool dbgNow = s_dbg && (s_call % 500 == 0); // periodic
    ++s_call;
    int liveN = 0, playN = 0, allocN = 0, waveN = 0, pauseN = 0;

    for (int ch = 0; ch < 64; ch++) {
        JASystem::DSPInterface::DSPBuffer* vpb = sb_jas_dspch_vpb(ch);
        HostVoice& hv = g_hostVoice[ch];
        if (vpb != nullptr) {
            ++allocN;
            if (vpb->unk118 != nullptr) ++waveN;
            if (vpb->unkC != 0) ++pauseN;
        }
        if (vpb == nullptr || vpb->unk118 == nullptr || vpb->unkC != 0 /*paused*/) {
            hv.playing = false;
            continue;
        }
        ++liveN;
        if (dbgNow && liveN <= 6) {
            std::fprintf(stderr,
                "[audio] ch%d wave=%p unk4(pitch)=%u unk64=%u unk100=%u unk11C(len)=%u "
                "unk10A=%u volL=%d volR=%d\n",
                ch, vpb->unk118, vpb->unk4, vpb->unk64, vpb->unk100, vpb->unk11C,
                vpb->unk10A, (int)(s16)vpb->unk10[0].targetVolume, (int)(s16)vpb->unk10[1].targetVolume);
        }
        const void* wb = vpb->unk118;
        if (wb != hv.waveBase) { // new wave bound to this channel -> (re)trigger
            hv.waveBase = wb;
            sb_decode_voice(vpb, hv);
            hv.cursor   = 0.0;
            hv.playing  = !hv.pcm.empty();
        }
        if (!hv.playing || hv.pcm.empty())
            continue;
        ++playN;

        const double step = static_cast<double>(vpb->unk4) / 4096.0; // unity = 4096 (JASChannel:898)
        if (step <= 0.0)
            continue;

        // Buses: unk10[0] = L, unk10[1] = R main (Q15 target volume). Aux/effects = M3.
        auto q15 = [](u16 v) -> float { s16 s = static_cast<s16>(v); return s > 0 ? static_cast<float>(s) / 32768.0f : 0.0f; };
        const float gL = q15(vpb->unk10[0].targetVolume);
        const float gR = q15(vpb->unk10[1].targetVolume);
        const size_t len = hv.pcm.size();
        double c = hv.cursor;
        for (u32 i = 0; i < n; i++) {
            const size_t idx = static_cast<size_t>(c);
            if (idx >= len) { hv.playing = false; break; } // v1: play-once (loop = next increment)
            const s16 s0 = hv.pcm[idx];
            const s16 s1 = (idx + 1 < len) ? hv.pcm[idx + 1] : s0;
            const float frac = static_cast<float>(c - static_cast<double>(idx));
            const float samp = s0 + (s1 - s0) * frac; // linear interpolation
            accL[i] += static_cast<s32>(samp * gL);
            accR[i] += static_cast<s32>(samp * gR);
            c += step;
        }
        hv.cursor = c;
    }

    if (s_dbg && liveN > s_maxLive) { // first time we see this many live voices
        s_maxLive = liveN;
        std::fprintf(stderr, "[audio] NEW max live voices=%d playing=%d (call %ld)\n", liveN, playN, s_call);
    }
    if (dbgNow)
        std::fprintf(stderr, "[audio] frame: alloc=%d wave=%d paused=%d live=%d playing=%d (call %ld)\n",
                     allocN, waveN, pauseN, liveN, playN, s_call);

    for (u32 i = 0; i < n; i++) {
        s32 l = accL[i], r = accR[i];
        L[i] = static_cast<s16>(l > 32767 ? 32767 : (l < -32768 ? -32768 : l));
        R[i] = static_cast<s16>(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
    }
}

} // namespace

// ---- other DSP-mailbox symbols the kernel pipeline pulls in --------------------
// All of these are real GC-DSP-hardware mailbox/register primitives with no PC
// counterpart. They are ALREADY documented no-op seams in
// sms-boot/boot_stubs/unresolved_stubs.cpp ("INTENTIONAL SEAM" block: DSPReleaseHalt,
// DsetMixerLevel, DsetupTable, DspBoot, DspFinishWork) — not redefined here. Only
// DsyncFrame2 moves here, because milestone 1 gives it a real (silence-producing,
// loud-once) body instead of a bare no-op.
void DsyncFrame2(u32 subframes, uintptr_t bufL, uintptr_t bufR)
{
    dsyncFrame2Native(subframes, bufL, bufR);
}

// Registered via Kernel::registerDacCallback(); fires once per Kernel::updateDac()
// call with the just-rendered dac[] buffer (see file header). Pushes straight to
// aurora::audio (opening the device lazily, skipped entirely under SB_HEADLESS).
void onDacBuffer(s16* buf, s32 numFrames)
{
    if (buf == nullptr || numFrames <= 0)
        return;
    // SB_AUDIO_RAW=<path>: append the interleaved s16 stereo DAC output to a raw
    // PCM file (persistent verification diagnostic — runs even headless, where the
    // device push is skipped). Analyze with python (RMS/peak) or `ffmpeg -f s16le
    // -ar 32000 -ac 2 -i <path> out.wav`. Milestone-2 audio A/B per the plan §M2.4.
    if (const char* rawPath = std::getenv("SB_AUDIO_RAW")) {
        static FILE* rawFp = std::fopen(rawPath, "wb");
        if (rawFp) {
            std::fwrite(buf, sizeof(s16) * 2, static_cast<size_t>(numFrames), rawFp);
            std::fflush(rawFp);
        }
    }
    if (g_headless)
        return;
    if (!g_audioOpen) {
        g_audioOpen = aurora_audio_open(32000, 2);
        if (!g_audioOpen)
            return; // device open failed — stay silent rather than crash on a host quirk.
    }
    aurora_audio_push(buf, static_cast<uint32_t>(numFrames));
}

extern "C" void sb_jas_kernel_init(void)
{
    if (g_inited)
        return;
    if (SMSGetMSound() == nullptr)
        return; // AudioThread::start() (-> Driver::init(), JASDram setup) hasn't run yet.

    g_headless = std::getenv("SB_HEADLESS") != nullptr;

    JASystem::Kernel::init();
    JASystem::Kernel::registerDacCallback(onDacBuffer);
    g_inited = true;

    if (std::getenv("SB_DBG_AUDIO"))
        std::fprintf(stderr, "[jas-native] Kernel::init() done (headless=%d)\n",
                     g_headless ? 1 : 0);
}

extern "C" void sb_jas_kernel_frame(void)
{
    if (!g_inited)
        return;

    static double sampleDebt = 0.0;
    static u64 frameCounter  = 0;
    static u64 updateDacCalls = 0;
    const bool dbg = std::getenv("SB_DBG_AUDIO") != nullptr;

    // Nominal NTSC output rate / 60 Hz video field — see cadence-math comment above.
    sampleDebt += 32000.0 / 60.0;
    const u32 frameSamples = JASystem::Kernel::getFrameSamples(); // 560
    while (sampleDebt >= frameSamples) {
        // PRODUCE one DSP frame: finishDSPFrame() -> process(UNK1) advances the
        // triple-buffer write pointer, runs DsyncFrame2 (the native voice mix) and
        // updateDSP() -> subframeCallback() (the SEQUENCER advance that triggers
        // note-ons -> voice allocation). This is the native equivalent of retail's
        // DSP-done interrupt driving finishDSPFrame. Without it the pipeline
        // DEADLOCKS: mixDSP()'s idle-kick only calls finishDSPFrame when dspstatus
        // == 0, but dspstatus is set to 1 by the first finishDSPFrame and only reset
        // inside finishDSPFrame's buffer-full branch (never reached), so the whole
        // producer path (sequencer + voice render) ran exactly ONCE per boot
        // (2026-07-17: startSeq fired 8x but DsyncFrame2 ran once, live voices=0).
        JASystem::DSPBuf::finishDSPFrame();
        // CONSUME: vframeWork() -> mixDSP() copies the produced dsp_buf -> dac[] and
        // fires the registered dac callback (-> aurora::audio).
        JASystem::Kernel::updateDac();
        sampleDebt -= frameSamples;
        ++updateDacCalls;
    }

    ++frameCounter;
    if (dbg && (frameCounter % 600) == 0) {
        std::fprintf(stderr, "[jas-native] frames=%llu updateDacs=%llu\n",
                     (unsigned long long)frameCounter,
                     (unsigned long long)updateDacCalls);
    }
}
