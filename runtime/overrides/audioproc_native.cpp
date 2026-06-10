// Native port of the JASystem audio thread main proc (JASystem::AudioThread::audioproc,
// func_80311170) — the dead-audio fix (2026-06-10).
//
// ROOT CAUSE. The audio thread's message loop has a defensive suicide: on a DSP frame-done
// message (msg 1) while intcount==0 it RETURNS, killing the thread (decomp
// JASAudioThread.cpp case 1: `if (intcount == 0) return nullptr;`). On hardware that state is
// impossible — the real DSP paces its 7 per-frame sub-frame mails (0xF355_FF00..FF06) across the
// 5 ms frame, so the last mail's msg can never outrun the receiver's own intcount bookkeeping.
// Under Dolphin's HLE the whole command list is processed INSTANTLY and the mails arrive
// back-to-back; one frame-done message lands after intcount already hit 0 → the thread silently
// exits a few seconds into boot. Downstream: syncAudio's audioproc_mq fills (16/16), every later
// OSSendMessage drops, the DSP command cycle stops (zero mails + kicks observed), DSP-done
// interrupts freeze at ~605, and ALL audio is silent after the first instant (the Nintendo-logo
// "first frame" bug and the post-THP silence are the same death).
//
// THE PORT. A faithful native replication of the recompiled body (same recompiled callees, same
// register protocol — diffable against generated func_80311170), with the HW-impossible state
// handled the way the hardware pipeline guarantees instead of by thread suicide: a frame-done
// message with intcount==0 is counted, logged once, and IGNORED (the frame already completed —
// finishDSPFrame ran when intcount hit 0; the extra message carries no work). msg 3 (stop) still
// exits faithfully. This is the ttrack_tick_native pattern: own the path natively where the
// recomp/HLE timing combination breaks a hardware assumption.
#include "../overrides.h"
#include "../intrinsics.h"
#include <cstdio>
#include <cstdlib>

#ifdef HAVE_DOLPHIN_CORE
extern u32 mem_r32(u32 ea);
extern void mem_w32(u32 ea, u32 v);

namespace {
// GMSE01 addresses (from the emitted func_80311170 / reference funcs list).
constexpr u32 MQ_AUDIOPROC   = 0x803FD858u;  // audioproc_mq (msgbuf at +0x20, 16 messages)
constexpr u32 OS_INIT_MSGQ   = 0x80346130u;  // OSInitMessageQueue
constexpr u32 OS_RECV_MSG    = 0x80346258u;  // OSReceiveMessage
constexpr u32 KERNEL_INIT    = 0x80315d40u;  // Kernel::init
constexpr u32 DSP_BOOT       = 0x803374c0u;  // DspBoot(syncDSP)
constexpr u32 DRIVER_INIT    = 0x803140ccu;  // Driver::init
constexpr u32 AI_SET_RATE    = 0x803526acu;  // AISetDSPSampleRate
constexpr u32 AI_REG_DMACB   = 0x803524c8u;  // AIRegisterDMACallback
constexpr u32 AI_START_DMA   = 0x80352594u;  // AIStartDMA
constexpr u32 UPDATE_DAC     = 0x80315fc0u;  // Kernel::updateDac      (msg 0)
constexpr u32 PROBE_START    = 0x803194f4u;  // Kernel::probeStart
constexpr u32 PROBE_FINISH   = 0x803194f8u;  // Kernel::probeFinish
constexpr u32 UPDATE_DSP     = 0x80314160u;  // DSPBuf::updateDSP      (msg 1, mid-frame)
constexpr u32 FINISH_FRAME   = 0x803141d8u;  // DSPBuf::finishDSPFrame (msg 1, intcount→0)
constexpr u32 OS_EXIT_THREAD = 0x80348a68u;  // OSExitThread           (msg 3)
// r13(=0x804141C0)-relative globals, from the emitted body:
constexpr u32 G_AI_SETTING   = 0x804141C0u - 23348;  // Kernel::gAiSetting (AISetDSPSampleRate arg)
constexpr u32 G_MQ_INIT      = 0x804141C0u - 23660;  // audioproc_mq_init
constexpr u32 G_DSP_BOOTED   = 0x804141C0u - 23664;  // isDSPBoot
constexpr u32 G_INTCOUNT     = 0x804141C0u - 23656;  // intcount (DSP sync count)

}
bool g_in_audioproc = false;   // tail_ppc names the handoff that unwinds this frame (diag)
namespace {
unsigned long g_stray_framedone = 0;   // HW-impossible frame-done msgs tolerated (diag)
constexpr u32 kRetPC = 0x80311170u;    // benign lr for recompiled callees (all callees are recomp)

void call1(CPUState& cpu, u32 fn, u32 r3) {
    cpu.gpr[3] = r3;
    cpu.lr = kRetPC;
    call_ppc(cpu, fn);
}

void ov_audioproc(CPUState& cpu) {
    g_in_audioproc = true;
    // Carve a guest stack frame like the original prologue (stwu r1,-0x18(r1) there; 0x40 here
    // for the msg slot) — callees build their frames BELOW r1, so locals must live above it.
    cpu.gpr[1] -= 0x40;
    // ── Init phase: faithful to the recomp body (same calls, same order) ──
    // GQR2..5 setup for the mixer's paired-single quantizers.
    cpu.gqr[2] = 0x00040004u; cpu.gqr[3] = 0x00050005u;
    cpu.gqr[4] = 0x00060006u; cpu.gqr[5] = 0x00070007u;
    {   // OSInitMessageQueue(&mq, msgbuf=mq+0x20, 16)
        CPUState c = cpu;
        c.gpr[3] = MQ_AUDIOPROC; c.gpr[4] = MQ_AUDIOPROC + 32; c.gpr[5] = 16;
        c.lr = kRetPC;
        call_ppc(c, OS_INIT_MSGQ);
    }
    mem_w32(G_MQ_INIT, 1);                       // audioproc_mq_init = true
    { CPUState c = cpu; call1(c, KERNEL_INIT, c.gpr[3]); }
    if (mem_r32(G_DSP_BOOTED) == 0) {
        CPUState c = cpu;
        c.gpr[3] = 0x80310000u + 4816;           // syncDSP (0x803112d0), as the emitted body loads
        c.lr = kRetPC;
        call_ppc(c, DSP_BOOT);
        mem_w32(G_DSP_BOOTED, 1);
    }
    { CPUState c = cpu; c.lr = kRetPC; call_ppc(c, DRIVER_INIT); }
    { CPUState c = cpu; call1(c, AI_SET_RATE, mem_r32(G_AI_SETTING)); }
    { CPUState c = cpu; call1(c, AI_REG_DMACB, 0x80310000u + 4336); }       // syncAudio (0x803110f0)
    { CPUState c = cpu; c.lr = kRetPC; call_ppc(c, AI_START_DMA); }

    // ── Message loop ──
    // Guest scratch for the received message: use the thread's own stack red zone.
    const u32 msg_slot = cpu.gpr[1] + 8;
    for (;;) {
        {   // OSReceiveMessage(&mq, &msg, OS_MESSAGE_BLOCK)
            CPUState c = cpu;
            c.gpr[3] = MQ_AUDIOPROC; c.gpr[4] = msg_slot; c.gpr[5] = 1;
            c.lr = kRetPC;
            call_ppc(c, OS_RECV_MSG);
        }
        const s32 msg = (s32)mem_r32(msg_slot);
        { static unsigned long n = 0; ++n;
          if (n <= 20 || (n % 2000) == 1)
              fprintf(stderr, "[audioproc] msg#%lu = %d intcount=%u\n", n, msg, mem_r32(G_INTCOUNT)); }
        if (msg == 0) {                          // AID frame → mix/update the DAC
            CPUState c = cpu; c.lr = kRetPC; call_ppc(c, UPDATE_DAC);
        } else if (msg == 1) {                   // DSP sub-frame done
            const u32 intcount = mem_r32(G_INTCOUNT);
            if (intcount == 0) {
                // HW-impossible (the original body EXITS THE THREAD here — the dead-audio bug
                // under Dolphin's instant HLE mails). The frame already finished; the stray
                // message carries no work. Tolerate, count, log once.
                if (g_stray_framedone++ == 0)
                    fprintf(stderr, "[audioproc] stray DSP frame-done with intcount==0 — "
                                    "tolerated (original guest body would kill the audio thread)\n");
                continue;
            }
            mem_w32(G_INTCOUNT, intcount - 1);
            if (intcount - 1 == 0) {
                CPUState c = cpu; call1(c, PROBE_FINISH, 7);
                CPUState f = cpu; f.lr = kRetPC; call_ppc(f, FINISH_FRAME);
            } else {
                CPUState c = cpu;
                c.gpr[3] = 2;
                c.gpr[4] = cpu.gpr[2] + 1672;        // probeStart(2, "SFR_DSP") — r2+1672 per body
                c.lr = kRetPC;
                call_ppc(c, PROBE_START);
                CPUState u = cpu; u.lr = kRetPC; call_ppc(u, UPDATE_DSP);
                CPUState p = cpu; call1(p, PROBE_FINISH, 2);
            }
        } else if (msg == 3) {                   // stop(): faithful thread exit
            fprintf(stderr, "[audioproc] msg 3 (stop) received — exiting audio thread\n");
            CPUState c = cpu; call1(c, OS_EXIT_THREAD, 0);
            return;
        }
        // msg 2 / anything else: continue (faithful)
    }
}

static const bool s_registered = [] {
    register_override(0x80311170u, &ov_audioproc);
    fprintf(stderr, "[audioproc] native audio-thread proc registered "
                    "(stray frame-done tolerated instead of thread suicide)\n");
    return true;
}();
}  // namespace
#endif
