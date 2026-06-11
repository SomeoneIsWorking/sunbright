// Native JAS audio frame driver — full ownership of the audio logic chain.
//
// WHY: the steady-state JAS cycle on hardware is interrupt+mail ping-pong:
//   AID IRQ → syncAudio → msg0 → updateDac
//   DSP subframe-ack mail → syncDSP → msg1 → audioproc → updateDSP ×6 / finishDSPFrame
// Under the hybrid, every hop (IRQ delivery, mail interrupt, message queue, thread wake) is a
// separate seam that has independently failed: time-parked CoreTiming events, EE-masked flush
// windows, intcount races, ucode self-halts. Each fix exposed the next hop (whack-a-mole).
//
// WHAT: replace the asynchronous plumbing with a direct, synchronous driver. All the mails are
// already synchronous on our port (DSPHLE HandleMail runs inside the guest's mailbox MMIO
// write), so calling the same guest functions in the same order on one thread reproduces the
// cycle exactly, with zero interrupts/messages/threads involved:
//   per DMA period (~560 samples @ 32028 Hz, paced by the native sink's fill):
//     Kernel::updateDac()        (80315fc0)  — AIInitDMA advance + vframeWork (ring read, DAC
//                                              mix, streams) + HardStream::main
//     DSPBuf::finishDSPFrame()   (803141d8)  — submits the next frame (cmd02 mails → our native
//                                              ucode, synchronously) + first updateDSP
//     DSPBuf::updateDSP() ×6     (80314160)  — per-subframe channel update + DSPReleaseHalt
//                                              (voice-map mails → our ucode renders a subframe)
// The audioproc thread parks forever in OSReceiveMessage (no more msg0/msg1 — harmless); the
// ucode's ack mails are suppressed (no consumer); AID delivery stops (the driver IS the
// cadence). DSP-task switching mails (CDD1/DCD1, ucode swap) keep the legacy capture path.
//
// ENGAGE: on the first render command (cmd02) seen by the native ucode — before that, boot
// handshake (DspBoot, DsetupTable busy-waits) runs the legacy mail+interrupt path unchanged.
#include "../overrides.h"
#include "../intrinsics.h"
#include "../dolphin_hook.h"
#include "Core/System.h"
extern u32 mem_r32(u32 ea);
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#ifdef HAVE_DOLPHIN_CORE
extern "C" long sunbright_audio_fill_ms();         // native_audio.cpp
extern "C" uint64_t na_consumed_frames();          // native_audio.cpp (device master clock)
u32 sunbright_idle_spin_pc();                      // dolphin_hook.cpp
extern thread_local CPUState* g_cur_recomp_cpu;

namespace {
constexpr u32 OS_SEND_MESSAGE  = 0x80346190u;      // OSSendMessage
constexpr u32 MQ_AUDIOPROC     = 0x803FD858u;      // JASystem audioproc_mq
constexpr u32 G_INTCOUNT       = 0x804141C0u - 23656u;  // AudioThread intcount
constexpr int kSubframes       = 7;                // SMS: Kernel::getSubFrames()

std::atomic<bool> g_engaged{false};
unsigned long g_periods = 0;
}  // namespace

bool sunbright_jas_driver_engaged() { return g_engaged.load(std::memory_order_relaxed); }

// Called by the native ucode on the first render command: the steady-state cycle exists now.
void sunbright_jas_driver_engage() {
    if (!g_engaged.exchange(true, std::memory_order_relaxed))
        fprintf(stderr, "[jasdrv] native JAS frame driver engaged (mails/IRQ/audioproc out of "
                        "the audio loop)\n");
}

// One DMA period of the JAS cycle. v1 ran updateDac/updateDSP synchronously on the calling
// thread — WRONG threading model: on hardware that work runs on the dedicated audioproc
// thread; only the message post runs in ISR context. Nesting the full DAC/sequencer inside
// arbitrary game threads corrupted TApplication/TVideo state (wild read in waitForRetrace).
// v2 reproduces the hardware message stream instead: per period, post msg0 (→ updateDac) and,
// when a full frame is in flight (intcount==7, all previous subframe msgs consumed), a batch
// of 7 msg1s (→ 6× updateDSP + finishDSPFrame). The original recompiled audioproc loop does
// the work on its own thread/stack with proper scheduling. The intcount==0 exit branch is
// unreachable: msg1s are only ever sent as full batches matching a fresh intcount of 7; a
// ring-full stall (intcount stuck 0) is restarted by updateDac→mixDSP's own
// dspstatus==0→finishDSPFrame path.
static bool send_msg(const CPUState* seed, u32 msg) {
    auto& ppc = Core::System::GetInstance().GetPPCState();
    CPUState c;
    if (seed) c = *seed;
    else      dolphin_state_to_cpu(ppc, c);
    const u32 saved_msr = ppc.msr.Hex;
    ppc.msr.Hex &= ~0x8000u;
    c.lr = sunbright_idle_spin_pc();
    c.gpr[3] = MQ_AUDIOPROC;
    c.gpr[4] = msg;
    c.gpr[5] = 0;                                    // OS_MESSAGE_NOBLOCK
    call_ppc(c, OS_SEND_MESSAGE);
    ppc.msr.Hex = saved_msr;
    return c.gpr[3] != 0;                            // 0 = queue full
}

static void run_one_period(const CPUState* seed) {
    send_msg(seed, 0);                               // updateDac (AID-tick equivalent)
    if (mem_r32(G_INTCOUNT) == (u32)kSubframes) {    // fresh frame, previous batch consumed
        for (int i = 0; i < kSubframes; i++)
            if (!send_msg(seed, 1)) break;
    }
    g_periods++;
}

// Pump — called from the device-service seam (poll_yield). Device-clocked, PC-game style:
// exactly one JAS period per DMA-period's worth of device time (~17.5 ms = 839 output frames
// @48 kHz), plus a small priming lead so the transport ring starts cushioned. Pacing on the
// sink FILL was wrong: fill responds to the AudioDMA transport, not to driver periods, so the
// driver free-ran the whole DAC/sequencer at ~75× (the one-blip-then-silence run).
int sunbright_jas_driver_pump(const CPUState* seed) {
    if (!g_engaged.load(std::memory_order_relaxed)) return 0;
    constexpr uint64_t kOutFramesPerPeriod = 48000ull * 560ull / 32028ull;   // ≈839
    constexpr uint64_t kPrimePeriods = 6;            // ~105 ms transport cushion at start
    static uint64_t t0 = 0;
    static bool t0_set = false;
    if (!t0_set) { t0 = na_consumed_frames(); t0_set = true; }
    const uint64_t due = (na_consumed_frames() - t0) / kOutFramesPerPeriod + kPrimePeriods;
    int n = 0;
    while (g_periods < due && n < 16) {
        run_one_period(seed);
        n++;
    }
    static const bool dbg = getenv("SUNBRIGHT_DBG_JASDRV") != nullptr;
    if (dbg) {
        static time_t last = 0;
        const time_t now = time(nullptr);
        if (now != last) {
            last = now;
            fprintf(stderr, "[jasdrv] periods=%lu fill=%ldms\n", g_periods,
                    sunbright_audio_fill_ms());
        }
    }
    return n;
}
#else
bool sunbright_jas_driver_engaged() { return false; }
void sunbright_jas_driver_engage() {}
int sunbright_jas_driver_pump(const CPUState*) { return 0; }
#endif
