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
#include <JSystem/JAudio/JASystem/JASRate.hpp>
#include <JSystem/dspproc.h>
#include <JSystem/dsptask.h>
#include <MSound/MSound.hpp>
#include <dolphin/types.h>

#include <aurora/audio.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

bool g_headless = false;
bool g_inited   = false;
bool g_audioOpen = false;

// Milestone-1 seam: real GC-DSP-ucode voice render (Zelda ucode) — see the file
// header. Zeroes the frame so downstream mixing sees silence, not garbage.
// bufL/bufR are live host pointers (uintptr_t — see JSystem/dsptask.h and the
// JASDSPBuf.cpp callsite comment: a u32 here truncates a real LP64 heap pointer
// and segfaults; verified live, not a hypothetical).
void dsyncFrame2Native(u32 /*subframes*/, uintptr_t bufL, uintptr_t bufR)
{
    static bool warned = false;
    if (!warned) {
        std::fprintf(stderr, "[STUB-CALLED] DsyncFrame2 (audio M2 seam)\n");
        warned = true;
    }
    const u32 frameSamples = JASystem::Kernel::getFrameSamples();
    std::memset(reinterpret_cast<void*>(bufL), 0, frameSamples * sizeof(s16));
    std::memset(reinterpret_cast<void*>(bufR), 0, frameSamples * sizeof(s16));
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
    if (g_headless || buf == nullptr || numFrames <= 0)
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
