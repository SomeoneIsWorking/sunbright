// Native AID (AI-DMA) interrupt chain — full ownership of the link that starves.
//
// THE BUG (2026-06-11): the JAS audio tick is msg-0 driven: AID IRQ → __AIDHandler → registered
// callback (JAS syncAudio / THP's own) → OSSendMessage(audioproc_mq, 0) → updateDac. Dolphin
// raises AID as DSP_CONTROL bit 3 + PI cause, and delivery then depends on the PI dispatch
// reaching the guest handler — measured to starve at the logo transition (/nintr intr7 ~2.5/s
// vs ~57/s) while UpdateAudioDMA kept pushing samples; voices froze at their last state
// (constant-DC pushes, perceived silence — the dead-jingle bug). The oracle plays fine, so the
// defect is delivery, not the game.
//
// THE PORT — own every link natively, PI/CoreTiming/dispatch-order independent:
//   raise    — linker --wrap on DSP::DSPManager::GenerateDSPInterrupt strips INT_AID before
//              Dolphin can set DSP_CONTROL/PI (the guest never sees a competing AID path) and
//              counts it host-side. Cadence stays exactly Dolphin's UpdateAudioDMA NumBlocks
//              wrap = the hardware contract.
//   delivery — sunbright_aid_pump (called from the time-independent device-service seam,
//              poll_yield) is the native __AIDHandler: per pending AID it calls the LIVE
//              registered AI-DMA callback (global r13-0x5874 = 0x8040B94C, set by
//              AIRegisterDMACallback 0x803524c8 — read live because THP re-registers it).
//   mails    — the same pump drains the DSP→CPU mailbox (CC005000 bit15) through the native
//              syncDSP port, so the per-subframe mail chain (updateDSP self-sustaining loop)
//              also never depends on PI dispatch. The guest ISR path, when it still runs,
//              finds the mailbox empty and no-ops (syncdsp_native checks once, never spins).
#include "../overrides.h"
#include "../intrinsics.h"
#include "../dolphin_hook.h"
#include "Core/System.h"
#include "Core/HW/DSP.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#ifdef HAVE_DOLPHIN_CORE
extern u32 mem_r32(u32 ea);
extern u16 mem_r16(u32 ea);
u32 sunbright_idle_spin_pc();                         // dolphin_hook.cpp (clean ISR return PC)
bool sunbright_deliver_pending_recomp(u32 logical_msr);  // dolphin_hook.cpp (native dispatch)
extern thread_local CPUState* g_cur_recomp_cpu;       // memory_bridge.h

namespace {
constexpr u32 AID_CALLBACK_GLOBAL = 0x8040E94Cu;      // AIRegisterDMACallback storage (r13-0x5874)
std::atomic<unsigned> g_aid_due{0};
unsigned long g_aid_delivered = 0, g_mail_drained = 0; // diag
}  // namespace

extern "C" void __real__ZN3DSP10DSPManager20GenerateDSPInterruptEml(void* self, u64 t, s64 c);
extern "C" void __wrap__ZN3DSP10DSPManager20GenerateDSPInterruptEml(void* self, u64 t, s64 c) {
    if (t & 0x08u) {                                   // INT_AID: owned natively, never reaches PI
        g_aid_due.store(1, std::memory_order_relaxed);
        t &= ~0x08ull;
        if (!t) return;
    }
    __real__ZN3DSP10DSPManager20GenerateDSPInterruptEml(self, t, c);
}

// The raise actually reaches DSP_CONTROL through UpdateAudioDMA, which INLINES
// GenerateDSPInterrupt (verified: objdump shows a direct SetInterrupt call, no call to the
// wrapped symbol — the wrap above only catches out-of-TU callers). So claim the AID at the
// UpdateAudioDMA seam instead (cross-TU, called from SystemTimers' AudioDMACallback): run the
// real DMA step, then if it latched the AID status bit, ACK it exactly as the guest ISR would
// (DSP_CONTROL write-1-to-clear, other ack bits preserved-unacked, mask bits untouched — this
// also clears the PI cause via the MMIO handler) and queue one native delivery.
extern void mem_w16(u32 ea, u16 v);
extern "C" void __real__ZN3DSP10DSPManager14UpdateAudioDMAEv(void* self);
extern "C" void __wrap__ZN3DSP10DSPManager14UpdateAudioDMAEv(void* self) {
    __real__ZN3DSP10DSPManager14UpdateAudioDMAEv(self);
    const u16 ctrl = mem_r16(0xCC00500Au);
    if (ctrl & 0x0008u) {                              // AID status latched by this DMA step
        mem_w16(0xCC00500Au, (u16)((ctrl & ~(0x0080u | 0x0020u)) | 0x0008u));
        // FLAG, not a counter: AID is level-triggered on hardware — back-to-back completions
        // during a masked/busy stretch coalesce into ONE delivery. Bursting the backlog as
        // N rapid updateDac ticks desynced the JAS↔ucode frame/yield cycle ("Sync mail
        // received when rendering was not active. Halting.", dsplog_r1).
        g_aid_due.store(1, std::memory_order_relaxed);
    }
}

// DSP-mail interrupts (INT_DSP, also ARAM): the DSP HLE raises them through
// GenerateDSPInterruptFromDSPEmu, which schedules a ≥500-cycle CoreTiming event — the same
// time-parked delivery disease as CP/PE/AID (governor stops Advance → event never fires →
// PI cause 7 never raised → the SDK DSP-task handshake AND the JAS subframe mail chain both
// starve; this was the measured intr7 stall). Own it: count the due interrupt bits here and
// let the pump apply them synchronously (DSP_CONTROL + PI) at the service seam, where
// native_dispatch_pending delivers the guest __DSPHandler in the same pass — the SDK then
// consumes the mailbox through its own unmodified protocol (handshake, task switch, req_cb).
namespace { std::atomic<u32> g_dspemu_due{0}; }
extern "C" void __real__ZN3DSP10DSPManager30GenerateDSPInterruptFromDSPEmuENS_16DSPInterruptTypeEi(
    void* self, int type, int cycles_into_future);
extern "C" void __wrap__ZN3DSP10DSPManager30GenerateDSPInterruptFromDSPEmuENS_16DSPInterruptTypeEi(
    void* self, int type, int cycles_into_future) {
    (void)self; (void)cycles_into_future;
    g_dspemu_due.fetch_or((u32)type, std::memory_order_relaxed);
}

// Synchronous flush at the post-store boundary. Called by the memory bridge right AFTER a guest
// CPU→DSP mailbox-low write returns (the write that triggered HandleMail → PushMail(interrupt) →
// the capture above). Hardware delivers the DSP interrupt within microseconds of the mail;
// deferring to the poll_yield pump made every DSP-mail round-trip wait for the next poll
// (~1.5 s per render cycle vs ~30/s — audio at 1/40 speed, perceived silence). Flushing INSIDE
// HandleMail (from the wrap itself) re-entered the HLE mid-update and deadlocked boot — the
// boundary must be after the MMIO write completes. native_dispatch_pending's own guard makes a
// nested call from an already-dispatching context a safe no-op (stays pending for the next round).
void sunbright_dsp_flush_sync() {
    if (!g_cur_recomp_cpu) return;                     // no live guest context: pump handles it
    // EE-off = critical section (hardware truth: GC ISRs and the JAS mail engine run with
    // interrupts masked, which serializes ALL mail traffic). Dispatching here anyway nested a
    // syncDSP/updateDSP mail send inside another thread's in-flight mail sequence → torn mails
    // (a CDD1 high half paired with a command-param low half, "cdd17ac0") → protocol desync.
    // Leave it captured; the pump delivers after EE is restored.
    if (!(Core::System::GetInstance().GetPPCState().msr.Hex & 0x8000u)) return;
    if (const u32 t = g_dspemu_due.exchange(0, std::memory_order_relaxed)) {
        __real__ZN3DSP10DSPManager20GenerateDSPInterruptEml(
            &Core::System::GetInstance().GetDSP(), t, 0);
        g_mail_drained++;
        sunbright_deliver_pending_recomp(0);
    }
}

// Native __AIDHandler + DSP-mail drain. ISR semantics mirrored from the PE token drain:
// EE masked for the handler body, seed register file from the live recomp context.
int sunbright_aid_pump(const CPUState* seed) {
    auto& ppc = Core::System::GetInstance().GetPPCState();
    int n = 0;
    if (g_aid_due.exchange(0, std::memory_order_relaxed)) {
        const u32 cb = mem_r32(AID_CALLBACK_GLOBAL);
        if (cb) {
        CPUState c;
        if (seed) c = *seed;
        else      dolphin_state_to_cpu(ppc, c);
        c.lr = sunbright_idle_spin_pc();
        const u32 saved_msr = ppc.msr.Hex;
        ppc.msr.Hex &= ~0x8000u;
        call_ppc(c, cb);
        ppc.msr.Hex = saved_msr;
        g_aid_delivered++;
        n++;
        }
    }
    // DSP-mail/ARAM interrupts captured from the HLE (see the FromDSPEmu wrap): apply the
    // DSP_CONTROL/PI raise NOW — native_dispatch_pending (which runs right after this pump in
    // poll_yield) then delivers the guest __DSPHandler in the same pass. NOT a hand-rolled
    // mailbox drain: consuming DMBH ourselves stole the DspBoot handshake mails (boot crash,
    // aidfix4) — the SDK's own ISR must do the reading.
    if (const u32 t = g_dspemu_due.exchange(0, std::memory_order_relaxed)) {
        __real__ZN3DSP10DSPManager20GenerateDSPInterruptEml(
            &Core::System::GetInstance().GetDSP(), t, 0);
        g_mail_drained++;
        n++;
    }
    // SUNBRIGHT_DBG_AID=1: per-second chain liveness — AID callbacks delivered, mails drained,
    // live callback ptr, JAS intcount. The link that stops moving is the one that died.
    static const bool dbg = getenv("SUNBRIGHT_DBG_AID") != nullptr;
    if (dbg) {
        static time_t last = 0;
        const time_t now = time(nullptr);
        if (now != last) {
            last = now;
            fprintf(stderr, "[aid] delivered=%lu mails=%lu cb=%08x intcount=%u due=%u\n",
                    g_aid_delivered, g_mail_drained, mem_r32(AID_CALLBACK_GLOBAL),
                    mem_r32(0x804141C0u - 23656u), g_aid_due.load(std::memory_order_relaxed));
        }
    }
    return n;
}
#else
int sunbright_aid_pump(const CPUState*) { return 0; }
#endif
