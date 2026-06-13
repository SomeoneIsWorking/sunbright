// native_dsp_regs.cpp — PC-native ownership of the DSP register block 0xCC005000-0xCC00500B
// (CPU<->DSP mailboxes + the DSP CSR), opt-in via SUNBRIGHT_NATIVE_DSPREGS=1, default OFF.
//
// =====================================================================================
// WHY  — the black box that already bit us
// =====================================================================================
// The DSP register window at 0xCC005000 is the seam between the GameCube SDK's audio
// task layer and the DSP/AI hardware. Dolphin's Source/Core/Core/HW/DSP.cpp owns these
// registers (m_dsp_control = the CSR, the HLE mailbox state inside m_dsp_emulator) and
// services them only through MMIO::Mapping handlers that honor MSR.DR (BAT translation).
// That gave us three classes of pain, ALL rooted in "the register read/write went
// through Dolphin's MMU/MMIO path under a transient guest MSR or at the wrong boundary":
//
//   1. "Unable to resolve read cc00500a" — a CoreTiming device callback (or a mid-rfi
//      access) reads DSP_CONTROL while MSR.DR is momentarily clear; Dolphin's
//      ReadFromHardware misses every case and returns garbage 0. The memory_bridge
//      mmio_r<T> already special-cases this (resolve via the MMIO mapping ignoring DR),
//      but a register read has NO bus side effect — it should just come from a native
//      mirror that cannot miss. (memory_bridge.cpp:266-292 documents the workaround.)
//   2. Torn mail (e.g. "cdd17ac0" = a CDD1 high half paired with a command-param low
//      half): nested mail sends interleaved when a DSP-mail interrupt was flushed into
//      an EE-masked critical section. (aid_native.cpp / sms_os_intr.cpp own the timing.)
//   3. Dead-audio / swallowed DSP-done: a guest read-modify-write CSR ack (CSR |= 0x2
//      to kick the CPU->DSP mailbox) write-1-cleared a PENDING-but-undispatched DSPINT
//      status bit, so the kernel waited forever on a DSP-done that was acked by accident
//      (dolphin_hook.cpp:1140-1149). The W1C protection lives in memory_bridge today.
//
// This file consolidates the CSR/mailbox *register semantics* into one native model so
// the registers are translation-independent (no MSR/MMU dependence) and no mailbox
// interrupt is lost. It does NOT reimplement the DSP itself or the AID/DAC-ucode chain —
// those are already owned (aid_native.cpp, zelda_ucode_native.cpp, native_ai_dma) and
// this file COORDINATES with them (consults sunbright_dsp_ack_allowed /
// sunbright_jas_driver_engaged; never duplicates GenerateDSPInterrupt).
//
// =====================================================================================
// RE — the GameCube side (reference/sms)
// =====================================================================================
// __DSPRegs[32] @ 0xCC005000 (u16 each):  (reference/sms/include/dolphin/hw_regs.h)
//   [0]=MAILBOX_IN_HI  [1]=MAILBOX_IN_LO   CPU->DSP mail (DSPSendMailToDSP writes both)
//   [2]=MAILBOX_OUT_HI [3]=MAILBOX_OUT_LO  DSP->CPU mail (DSPReadMailFromDSP reads both)
//   [5]=CONTROL_STATUS  -> the CSR at byte offset 0x500A (reg index 5 = +0x0A)
//   [9/11/13]=ARAM size/mode/refresh, [16..]=ARAM DMA, [24..]=audio DMA.
//
// dsp.c (reference/sms/src/dolphin/dsp/dsp.c):
//   DSPCheckMailToDSP()   = (__DSPRegs[0] & 0x8000) >> 15   // bit15 of MAILBOX_IN_HI = full
//   DSPCheckMailFromDSP() = (__DSPRegs[2] & 0x8000) >> 15   // bit15 of MAILBOX_OUT_HI = full
//   DSPReadMailFromDSP()  = (__DSPRegs[2] << 16) | __DSPRegs[3]
//   DSPSendMailToDSP(m)   { __DSPRegs[0]=m>>16; __DSPRegs[1]=m&0xFFFF; }
//   DSPAssertInt(): CSR = (CSR & ~0xA8) | 2     // set DSPAssertInt(bit1), clear status acks
//   DSPInit():      CSR = (CSR & ~0xA8) | 0x800 // set DSPInit(bit11); then CSR &= ~0xAC
//
// __DSPHandler (reference/sms/src/JSystem/osdsp_task.c, the int-7 handler):
//   entry:  CSR = (CSR & ~0x28) | 0x80          // clear AID(0x08)+ARAM(0x20) status,
//                                               // SET DSPINT(0x80) status to ack the DSP int
//   then spins DSPCheckMailFromDSP()==0, reads the mail, runs the DCD1xxxx task protocol,
//   replies via DSPSendMailToDSP(0xCDD1xxxx) and spins DSPCheckMailToDSP()!=0.
//
// CSR bit layout (matches Dolphin UDSPControl, DSP.h:43):
//   b0 DSPReset  b1 DSPAssertInt  b2 DSPHalt
//   b3 AID(status) b4 AID_mask  b5 ARAM(status) b6 ARAM_mask  b7 DSP(status) b8 DSP_mask
//   b9 DMAState  b10 DSPInitCode  b11 DSPInit
//   Interrupt asserted when ((CSR>>1) & CSR & (0x08|0x20|0x80)) != 0  (DSP.cpp UpdateInterrupts).
//   Status bits 3/5/7 are WRITE-1-TO-CLEAR; DSP_CONTROL_MASK=0x0C07 (b0,b1,b2,b10,b11) is
//   owned by the DSP emulator (DSP.cpp:254/261), the rest by m_dsp_control.
//
// =====================================================================================
// THE MODEL (this file)
// =====================================================================================
// A native CSR/mailbox mirror (g_csr + g_mbox[4]) that:
//   * READS  (0xCC005000-0x500B): served from the mirror — re-synced from Dolphin's
//     live state first when a guest context is present (so the mirror tracks the HLE),
//     but ALWAYS answerable even mid-rfi / real-mode (a register read has no side
//     effect; this kills the "Unable to resolve cc00500a" class structurally).
//   * WRITES to the CSR (0x500A): W1C the status bits (3/5/7) — but only bits the
//     currently-dispatching DSP handler is allowed to ack (sunbright_dsp_ack_allowed),
//     so a sibling status isn't swallowed (the dead-audio fix, made first-class here).
//     The accepted ack is forwarded to Dolphin so PI cause-7 deasserts; control bits
//     (reset/assert/halt/mask/init) pass through unchanged.
//   * MAILBOX writes (0x5000/0x5002 to-DSP): pass straight through to Dolphin's HLE
//     (its mailbox state machine is the DSP — we do NOT model it), and the existing
//     post-store DSP-mail flush (sunbright_dsp_flush_sync) still fires.
//
// Interrupt DELIVERY is NOT re-done here — it is already deterministic and time-parked-
// safe via aid_native.cpp (sunbright_aid_pump from poll_yield) + sms_os_intr.cpp (EE 0->1
// flush). This file only guarantees the *register* the guest reads/acks is coherent and
// never misses, and that the ack is W1C-correct. sunbright_native_dspregs_sync() is the
// mirror-refresh hook intended to run alongside the device-service seam (poll_yield), so
// the mirror is fresh even when no recomp read is in flight.
//
// =====================================================================================
// REQUIRED SEAM — apply manually to runtime/memory_bridge.cpp (do NOT auto-applied here)
// =====================================================================================
// The memory bridge must give this file first crack at reads AND writes of the DSP
// register window so the native mirror is authoritative. Both seams are no-ops unless
// SUNBRIGHT_NATIVE_DSPREGS=1 (the hooks return false / the env gate is off), so applying
// them is safe and inert by default.
//
//   // ==== REQUIRED SEAM (apply manually) ====
//   // In mem_r16_slow(u32 ea), AFTER `check_wild_read(ea, 16);` and BEFORE `const u16 v = MMIO_R(16, ea);`
//   //   add:
//   //     {
//   //         extern bool sunbright_native_dspregs_r16(u32 ea, u16* out);   // native_dsp_regs.cpp
//   //         u16 nv;
//   //         if (sunbright_native_dspregs_r16(ea, &nv)) return nv;
//   //     }
//   //
//   // In mem_r32_slow(u32 ea), AFTER `check_wild_read(ea, 32);` and BEFORE `const u32 v = MMIO_R(32, ea);`
//   //   add:
//   //     {
//   //         extern bool sunbright_native_dspregs_r32(u32 ea, u32* out);   // native_dsp_regs.cpp
//   //         u32 nv;
//   //         if (sunbright_native_dspregs_r32(ea, &nv)) return nv;
//   //     }
//   //
//   // In mem_w16_slow(u32 ea, u16 v), REPLACE the existing CSR W1C block:
//   //     if ((ea & 0x0FFFFFFEu) == 0x0C00500Au) { ... existing dspprot logic ... }
//   //   with:
//   //     {
//   //         extern bool sunbright_native_dspregs_w16(u32 ea, u16* v);     // native_dsp_regs.cpp
//   //         sunbright_native_dspregs_w16(ea, &v);   // rewrites v in place (W1C-protect); inert if disabled
//   //     }
//   //   (the existing dspprot path remains the fallback when SUNBRIGHT_NATIVE_DSPREGS is off,
//   //    because the hook leaves v untouched in that mode — keep the old block guarded by
//   //    `if (!sunbright_native_dspregs_enabled())` if you want both, or just route through the hook.)
//   //
//   // OPTIONAL (mirror freshness): in poll_yield (dolphin_hook.cpp), next to the
//   //   sunbright_aid_pump(...) call, add:
//   //     { extern void sunbright_native_dspregs_sync(); sunbright_native_dspregs_sync(); }
//   // ==== END REQUIRED SEAM ====
//
// =====================================================================================
// VERIFY (headless — no ears needed)
// =====================================================================================
//   1. Baseline OFF (default): a normal headless run still plays audio.
//        SUNBRIGHT_HEADLESS=1 SUNBRIGHT_DUMP_AUDIO=1 ./build/sunbright   # ~120s
//        python tools/audio/wav_rms.py scratch/wav/native_dsp.wav        # sustained RMS
//   2. ON: identical audio, no regressions, no resolve warnings.
//        SUNBRIGHT_HEADLESS=1 SUNBRIGHT_NATIVE_DSPREGS=1 SUNBRIGHT_DUMP_AUDIO=1 \
//          ./build/sunbright 2>scratch/logs/dspregs.log
//        - grep -c "Unable to resolve read cc00500a" scratch/logs/dspregs.log   # expect 0
//        - python tools/audio/wav_rms.py scratch/wav/native_dsp.wav             # RMS unchanged
//        - python tools/audio/raw_profile.py / loop_detect.py for dead-audio (constant-DC) check
//   3. Diag: SUNBRIGHT_DBG_DSPREGS=1 prints a per-second line (reads served, CSR acks,
//      acks W1C-protected, mailbox passthroughs) so a stuck link is visible.
//
// Build note: NEW file -> `cmake -B build` reconfigure before `cmake --build build`
// (CMakeLists file(GLOB) only re-evaluates at configure time).

#include "../overrides.h"
#include "../intrinsics.h"
#include "../dolphin_hook.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#ifdef HAVE_DOLPHIN_CORE
#include "Core/System.h"
#include "Core/HW/DSP.h"
#include "Core/PowerPC/PowerPC.h"

// Bridge primitives (memory_bridge.cpp) — go through Dolphin's MMIO so the HLE/PI side
// stays coherent. We use these only to (a) seed the mirror and (b) forward the accepted CSR
// ack so PI cause-7 deasserts. They are the SAME calls aid_native.cpp already uses.
extern u16 mem_r16(u32 ea);
extern void mem_w16(u32 ea, u16 v);

// Coordination with the existing native chain — consult, never fight.
// (C++ linkage: matches the real definition `u32 sunbright_dsp_ack_allowed()` in dolphin_hook.cpp;
//  the original draft declared it extern "C", which did not resolve against the mangled symbol.)
extern u32 sunbright_dsp_ack_allowed();          // dolphin_hook.cpp: status bits the in-flight
                                                 //   DSP handler may ack (0 = none)
extern bool sunbright_jas_driver_engaged();      // jas_driver_native.cpp (audio cadence owner)
extern thread_local CPUState* g_cur_recomp_cpu;  // memory_bridge.h: live guest context or null

namespace {

constexpr u32 DSP_BASE   = 0x0C005000u;   // masked (ea & 0x0FFFFFFFu)
constexpr u32 DSP_END    = 0x0C00500Cu;   // exclusive: covers regs [0..5] = bytes 0x5000..0x500B
constexpr u32 CSR_ADDR   = 0x0C00500Au;   // DSP_CONTROL byte address
constexpr u16 INT_STATUS = 0x00A8u;       // AID(0x08)|ARAM(0x20)|DSP(0x80) — the W1C status bits

bool enabled() {
    static const int v = []{
        const char* e = getenv("SUNBRIGHT_NATIVE_DSPREGS");
        return (e && *e && *e != '0') ? 1 : 0;
    }();
    return v != 0;
}

// Native register mirror. The CSR and the four mailbox halves are the only registers a
// real-mode/mid-rfi read can legitimately hit without a bus side effect; the rest of the
// window (ARAM/audio-DMA) is rare and falls through to Dolphin's handlers unchanged.
std::atomic<u16> g_csr{0};
std::atomic<bool> g_csr_valid{false};
u16 g_mbox[4] = {0, 0, 0, 0};             // [0]=in_hi [1]=in_lo [2]=out_hi [3]=out_lo
std::atomic<bool> g_mbox_valid{false};

// Diagnostics (SUNBRIGHT_DBG_DSPREGS=1).
unsigned long g_reads = 0, g_acks = 0, g_protected = 0, g_passthru = 0, g_resolve_saves = 0;

bool dbg() {
    static const bool v = getenv("SUNBRIGHT_DBG_DSPREGS") != nullptr;
    return v;
}

void dbg_tick() {
    if (!dbg()) return;
    static time_t last = 0;
    const time_t now = time(nullptr);
    if (now == last) return;
    last = now;
    fprintf(stderr,
            "[dspregs] reads=%lu csr_acks=%lu protected=%lu mbox_pass=%lu resolve_saves=%lu csr=%04x\n",
            g_reads, g_acks, g_protected, g_passthru, g_resolve_saves,
            g_csr.load(std::memory_order_relaxed));
}

// Pull Dolphin's live register state into the mirror. Only safe to touch the MMIO path when a
// guest context is in flight (msr present); a bare real-mode read with no context must NOT call
// into Dolphin's MMU (that is precisely the path that warns/returns garbage) — it answers from
// whatever the mirror last held. We refresh from Dolphin's mapping IGNORING MSR.DR via mem_r16,
// which the bridge already resolves register-only without honoring translation.
void refresh_from_dolphin() {
    if (!g_cur_recomp_cpu) return;            // no live context: keep the last mirror value
    // CSR: byte address 0x500A. mem_r16 routes through the bridge's register-resolving read.
    g_csr.store(mem_r16(0xCC00500Au), std::memory_order_relaxed);
    g_csr_valid.store(true, std::memory_order_relaxed);
    // Mailboxes (status bit15 = full; the SDK polls these). out_hi/out_lo carry the DSP reply.
    g_mbox[0] = mem_r16(0xCC005000u);
    g_mbox[1] = mem_r16(0xCC005002u);
    g_mbox[2] = mem_r16(0xCC005004u);
    g_mbox[3] = mem_r16(0xCC005006u);
    g_mbox_valid.store(true, std::memory_order_relaxed);
}

// Answer a single 16-bit register from the mirror. Returns true if served.
bool serve_r16(u32 lo, u16* out) {
    refresh_from_dolphin();
    switch (lo) {
        case 0x0C005000u: case 0x0C005002u:
        case 0x0C005004u: case 0x0C005006u:
            if (!g_mbox_valid.load(std::memory_order_relaxed)) { g_resolve_saves++; }
            *out = g_mbox[(lo - DSP_BASE) >> 1];
            g_reads++;
            return true;
        case CSR_ADDR:
            if (!g_csr_valid.load(std::memory_order_relaxed)) { g_resolve_saves++; }
            *out = g_csr.load(std::memory_order_relaxed);
            g_reads++;
            return true;
        default:
            return false;
    }
}

}  // namespace

// ── Read seam: 0xCC005000-0x500B served from the native mirror ──────────────────────────
extern "C" bool sunbright_native_dspregs_r16(u32 ea, u16* out) {
    if (!enabled()) return false;
    const u32 lo = ea & 0x0FFFFFFFu;
    if (lo < DSP_BASE || lo >= DSP_END) return false;
    dbg_tick();
    return serve_r16(lo, out);
}

extern "C" bool sunbright_native_dspregs_r32(u32 ea, u32* out) {
    if (!enabled()) return false;
    const u32 lo = ea & 0x0FFFFFFFu;
    if (lo < DSP_BASE || lo + 4u > DSP_END) return false;   // must fit two served halves
    dbg_tick();
    // 32-bit DSP access = two 16-bit halves big-endian (Dolphin registers it the same way,
    // DSP.cpp ReadToSmaller). DSPReadMailFromDSP does exactly this 32-bit read of OUT_HI:OUT_LO,
    // and the misaligned 0x500A read (the resolve bug) lands here too.
    u16 hi, loh;
    if (!serve_r16(lo, &hi) || !serve_r16(lo + 2u, &loh)) return false;
    *out = ((u32)hi << 16) | loh;
    return true;
}

// ── Write seam: CSR W1C model; mailbox writes pass through ───────────────────────────────
// Rewrites *v in place. For the CSR (0x500A) it strips status bits the in-flight handler is not
// allowed to ack (W1C protection — the dead-audio fix), forwards the accepted value to Dolphin
// (mem_w16) so PI cause-7 deasserts and the HLE sees the control bits, and updates the mirror.
// For mailbox/other registers it does nothing (pass straight through to Dolphin's HLE; the
// mailbox state machine IS the DSP and we do not model it). Inert when disabled.
extern "C" bool sunbright_native_dspregs_w16(u32 ea, u16* v) {
    if (!enabled()) return false;
    const u32 lo = ea & 0x0FFFFFFEu;          // align to the 16-bit register
    if (lo != CSR_ADDR) {
        // Mailbox / ARAM / audio-DMA writes: pass through unchanged. Track to-DSP mailbox
        // passthroughs for the diag line; the actual store + post-store DSP-mail flush is the
        // memory bridge's job (sunbright_dsp_flush_sync remains wired there).
        if ((ea & 0x0FFFFFFFu) >= DSP_BASE && (ea & 0x0FFFFFFFu) < DSP_END) g_passthru++;
        return false;
    }
    dbg_tick();
    const u16 want = *v;
    // Live CSR (status bits currently latched by the HW/HLE).
    const u16 live = mem_r16(0xCC00500Au);
    const u16 pending = live & INT_STATUS;                 // AID|ARAM|DSP status set now
    const u16 allowed = (u16)sunbright_dsp_ack_allowed();  // bits the in-flight handler may ack
    const u16 protect = pending & (u16)~allowed;           // pending bits NOT yet dispatched
    u16 outv = want;
    if (want & protect) {
        // Guest tried to W1C a pending-but-undispatched sibling status — strip those W1C bits so
        // the interrupt is preserved (delivered later by aid_native's pump). Control + mask bits
        // and any legitimately-allowed ack pass through.
        outv = (u16)(want & ~protect);
        g_protected++;
    }
    if (want & pending & allowed) g_acks++;                // a real, permitted status ack
    // Forward to Dolphin: its CSR handler applies the W1C to m_dsp_control and calls
    // UpdateInterrupts() (deasserting PI cause-7 for the acked bit). This keeps Dolphin's PI
    // state and our mirror in lockstep.
    mem_w16(0xCC00500Au, outv);
    // Mirror reflects the post-write CSR (status bits cleared per W1C; control bits as written).
    g_csr.store(mem_r16(0xCC00500Au), std::memory_order_relaxed);
    g_csr_valid.store(true, std::memory_order_relaxed);
    *v = outv;                                             // hand the (possibly trimmed) value back
    // We have fully serviced the CSR write; tell the bridge to skip its own store of *v.
    return true;
}

// ── Mirror freshness hook (optional seam): refresh from Dolphin at the device-service seam ──
// Lets the mirror track HLE-driven CSR/mailbox changes even when no recomp read is in flight,
// so the very next real-mode read is fresh. Cheap; only refreshes when a guest context exists.
extern "C" void sunbright_native_dspregs_sync() {
    if (!enabled()) return;
    refresh_from_dolphin();
    dbg_tick();
}

extern "C" bool sunbright_native_dspregs_enabled() { return enabled(); }

#else  // !HAVE_DOLPHIN_CORE — standalone build: inert stubs so the seam links.
extern "C" bool sunbright_native_dspregs_r16(uint32_t, uint16_t*) { return false; }
extern "C" bool sunbright_native_dspregs_r32(uint32_t, uint32_t*) { return false; }
extern "C" bool sunbright_native_dspregs_w16(uint32_t, uint16_t*) { return false; }
extern "C" void sunbright_native_dspregs_sync() {}
extern "C" bool sunbright_native_dspregs_enabled() { return false; }
#endif
