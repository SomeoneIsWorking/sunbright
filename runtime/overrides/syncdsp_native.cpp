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
#include "../dolphin_hook.h"
#include "Core/System.h"
#include <atomic>
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
// Early frame-done mails (the intcount race, see ov_sync_dsp) parked until intcount>0.
std::atomic<unsigned> g_deferred_framedone{0};
bool g_frames_seen = false;     // first intcount>0 observed (first real DSP frame started)

void ov_sync_dsp(CPUState& cpu) {
    // Port of the emitted func_803112d0 (same callees, same flow) — with ONE deviation: the
    // original busy-waits for a mail (on HW the ISR only fires with one pending). Under native
    // ownership the aid_native pump drains mails at the service seam, so a guest-dispatched
    // call can find the mailbox already empty — return instead of spinning forever.
    CPUState c = cpu;
    c.lr = 0x803112e0u;
    call_ppc(c, DSP_CHECK_MAIL);
    if (c.gpr[3] == 0) return;                        // no mail: already drained natively
    c.lr = 0x803112ecu;
    call_ppc(c, DSP_READ_MAIL);                       // r3 = mail
    const u32 mail = c.gpr[3];

    if ((mail >> 16) != mem_r16(G_JAS_PREFIX)) return;   // not ours

    if ((mail & 0xFF00u) == 0xFF00u) {                // frame-done class
        if (mem_r32(G_MQ_INIT) != 0) {
            if (mem_r32(G_INTCOUNT) == 0) {
                // intcount==0 frame-done. Two cases, both HW-impossible-by-timing:
                //  · the single boot-time stray (DspBoot's instant HLE mail) — must be discarded
                //    (forwarding msg 1 makes audioproc's intcount==0 branch EXIT THE THREAD,
                //    the original dead-audio bug);
                //  · the intcount RACE: under nthr + instant HLE the mail's interrupt dispatch
                //    can run before the audioproc thread's setDSPSyncCount lands. The mail is
                //    REAL — dropping it kills the whole frame chain (each mail triggers the
                //    next updateDSP; one loss = no more mails ever, finishDSPFrame never runs,
                //    voices freeze at last state = the post-jingle constant-DC / silent-boot
                //    bug, 2026-06-11). DEFER it: sunbright_jas_flush_deferred forwards it the
                //    moment intcount>0; the boot stray is discarded there at the first frame
                //    transition instead of here (we cannot tell the two apart yet).
                g_deferred_framedone.fetch_add(1, std::memory_order_relaxed);
                if (g_stray_framedone++ < 4)
                    fprintf(stderr, "[syncdsp] frame-done mail %08x with intcount==0 — deferred "
                                    "(total %lu)\n", mail, g_stray_framedone);
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

}  // namespace

// Native syncDSP entry for the aid_native pump (mail already verified pending by the caller,
// but ov_sync_dsp re-checks and no-ops safely either way).
void sunbright_syncdsp_run(CPUState& cpu) { ov_sync_dsp(cpu); }

// Deferred frame-done flush — called from the time-independent device-service seam
// (sunbright_poll_yield), like the CP-interrupt and PE-token pumps: JAS mail delivery is the
// DSP's own act and must not depend on interrupt-dispatch/thread-schedule ordering. At the
// FIRST intcount>0 transition the single boot stray is discarded; afterwards every deferred
// mail is forwarded as the msg 1 the audioproc loop expects, as soon as it can be consumed
// safely (intcount>0).
int sunbright_jas_flush_deferred(const CPUState* seed) {
    using namespace ::std;
    if (!g_deferred_framedone.load(std::memory_order_relaxed)) return 0;
    if (mem_r32(G_INTCOUNT) == 0) return 0;
    if (!g_frames_seen) {
        g_frames_seen = true;
        const unsigned dropped = g_deferred_framedone.exchange(0, std::memory_order_relaxed);
        if (dropped > 1) g_deferred_framedone.store(dropped - 1, std::memory_order_relaxed);
        fprintf(stderr, "[syncdsp] first DSP frame started — discarded 1 boot stray "
                        "(%u were pending)\n", dropped);
        if (!g_deferred_framedone.load(std::memory_order_relaxed)) return 0;
    }
    int sent = 0;
    while (g_deferred_framedone.load(std::memory_order_relaxed) && mem_r32(G_INTCOUNT) != 0) {
        CPUState c;
        if (seed) c = *seed;
        else dolphin_state_to_cpu(Core::System::GetInstance().GetPPCState(), c);
        c.gpr[3] = MQ_AUDIOPROC;
        c.gpr[4] = 1;
        c.gpr[5] = 0;
        c.lr     = 0x80311334u;
        call_ppc(c, OS_SEND_MESSAGE);
        g_deferred_framedone.fetch_sub(1, std::memory_order_relaxed);
        sent++;
    }
    return sent;
}

namespace {
static const bool s_registered = [] {
    register_override(SYNC_DSP, &ov_sync_dsp);
    fprintf(stderr, "[syncdsp] native syncDSP registered "
                    "(stray frame-done dropped at the sender)\n");
    return true;
}();
}  // namespace
#endif
