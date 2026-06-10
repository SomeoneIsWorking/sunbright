// Native port of JASystem::AudioThread::syncDSP (0x803112d0) — the dead-audio fix, v2.
//
// ROOT CAUSE (unchanged from v1): the audioproc thread's message loop exits the thread on a DSP
// frame-done message (msg 1) while intcount==0 — a state impossible on hardware (the real DSP
// paces its per-frame sub-frame mails) but routine under Dolphin's instant HLE, killing ALL
// audio a few seconds into boot.
//
// v1 ported the whole audioproc thread body and tolerated the stray at the RECEIVER; that
// override owned too much (thread lifecycle, guest stack discipline, a dozen callees) and
// introduced its own NULL-deref crash. v2 is the minimal surface: port syncDSP — the ~20-line
// ISR leaf that SENDS the message — verbatim from the emitted func_803112d0, with one guard:
// a frame-done mail arriving while intcount==0 is counted and dropped instead of being
// forwarded as the thread-killing msg 1. The original recompiled audioproc body runs unmodified
// (its intcount==0 suicide branch simply becomes unreachable, as it is on hardware).
#include "../overrides.h"
#include "../intrinsics.h"
#include <cstdio>
#include <cstdlib>

#ifdef HAVE_DOLPHIN_CORE
extern u32 mem_r32(u32 ea);
extern u16 mem_r16(u32 ea);

namespace {
constexpr u32 SYNC_DSP          = 0x803112d0u;
constexpr u32 DSP_CHECK_MAIL    = 0x80353d48u;  // DSPCheckMailFromDSP
constexpr u32 DSP_READ_MAIL     = 0x80353d58u;  // DSPReadMailFromDSP
constexpr u32 OS_SEND_MESSAGE   = 0x80346190u;  // OSSendMessage
constexpr u32 DSP_RELEASE_HALT  = 0x803372e0u;  // DSPReleaseHalt
constexpr u32 DSP_FINISH_WORK   = 0x80337700u;  // DspFinishWork
constexpr u32 MQ_AUDIOPROC      = 0x803FD858u;  // audioproc_mq
constexpr u32 G_JAS_PREFIX      = 0x804141C0u - 29704;  // u16: expected mail hi (0xF355 at runtime)
constexpr u32 G_MQ_INIT         = 0x804141C0u - 23660;  // audioproc_mq_init
constexpr u32 G_INTCOUNT        = 0x804141C0u - 23656;  // intcount (expected frame-done mails)

unsigned long g_stray_framedone = 0;

void ov_sync_dsp(CPUState& cpu) {
    // Verbatim port of the emitted func_803112d0 (same callees, same flow).
    CPUState c = cpu;
    do {                                              // while (!DSPCheckMailFromDSP());
        c.lr = 0x803112e0u;
        call_ppc(c, DSP_CHECK_MAIL);
    } while (c.gpr[3] == 0);
    c.lr = 0x803112ecu;
    call_ppc(c, DSP_READ_MAIL);                       // r3 = mail
    const u32 mail = c.gpr[3];

    if ((mail >> 16) != mem_r16(G_JAS_PREFIX)) return;   // not ours

    if ((mail & 0xFF00u) == 0xFF00u) {                // frame-done class
        if (mem_r32(G_MQ_INIT) != 0) {
            if (mem_r32(G_INTCOUNT) == 0) {
                // HW-impossible stray (instant HLE mails): the frame already completed. The
                // original forwards msg 1 and audioproc's defensive `if (intcount==0) return`
                // KILLS THE AUDIO THREAD. Drop it instead — count + log once.
                if (g_stray_framedone++ == 0)
                    fprintf(stderr, "[syncdsp] stray frame-done mail %08x with intcount==0 — "
                                    "dropped (would have killed the audio thread)\n", mail);
                return;
            }
            c.gpr[3] = MQ_AUDIOPROC;                  // OSSendMessage(&mq, 1, NOBLOCK)
            c.gpr[4] = 1;
            c.gpr[5] = 0;
            c.lr = 0x80311334u;
            call_ppc(c, OS_SEND_MESSAGE);
        } else {
            c.lr = 0x8031133cu;
            call_ppc(c, DSP_RELEASE_HALT);
        }
    } else {
        c.gpr[3] = mail & 0xFFFFu;                    // DspFinishWork(mail & 0xFFFF)
        c.lr = 0x80311348u;
        call_ppc(c, DSP_FINISH_WORK);
    }
}

static const bool s_registered = [] {
    register_override(SYNC_DSP, &ov_sync_dsp);
    fprintf(stderr, "[syncdsp] native syncDSP registered "
                    "(stray frame-done dropped at the sender)\n");
    return true;
}();
}  // namespace
#endif
