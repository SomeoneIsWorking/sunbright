// ─────────────────────────────────────────────────────────────────────────────
// native_pi2.cpp — PC-native ownership of the Flipper Processor Interface (PI)
//                  interrupt controller: INTSR (cause) @ 0xCC003000 and INTMR
//                  (mask) @ 0xCC003004, plus the "is any unmasked cause pending"
//                  predicate the native dispatcher consults.
//
// OPT-IN: SUNBRIGHT_NATIVE_PI=1. Default OFF = current behavior (Dolphin's
// ProcessorInterfaceManager owns cause/mask; the dispatcher reads pi.GetCause()/
// GetMask()). This file is INERT unless the flag is set AND the seam below is
// applied — with the flag off it self-registers nothing and every entry point
// short-circuits to "not handled".
//
// This is a clean, NEW-FILE rewrite of scratch/native_drafts/native_pi.cpp. The
// RE is unchanged; only the integration approach differs (no existing-file edits;
// the two MMIO seams are documented below for the main session to apply by hand).
//
// ── What was reverse-engineered ──────────────────────────────────────────────
//
// Dolphin side — externals/dolphin/Source/Core/Core/HW/ProcessorInterface.cpp:
//   * m_interrupt_cause / m_interrupt_mask back PI_INTERRUPT_CAUSE (0x00) and
//     PI_INTERRUPT_MASK (0x04) (RegisterMMIO, L68-82).
//   * READ of cause/mask is a plain DirectRead<u32> of those fields (L70,77).
//   * WRITE to CAUSE is WRITE-1-TO-CLEAR: m_interrupt_cause &= ~val; UpdateException()
//     (L71-75). WRITE to MASK is a plain store of val; UpdateException() (L77-82).
//   * SetInterrupt(cause_mask,set) (L199-222): devices (CP/PE/DSP/AI/VI/DI/SI/EXI)
//     raise/lower their PI cause bit through here, then UpdateException().
//   * UpdateException() (L149-156): if (cause & mask) != 0 → set
//     EXCEPTION_EXTERNAL_INT, else clear it. THIS is how PI raises the guest
//     external interrupt — and our recomp-boundary delivery gates on it.
//   * INT_CAUSE_* (ProcessorInterface.h L30-45): PI=0x1 RSW=0x2 DI=0x4 SI=0x8
//     EXI=0x10 AI=0x20 DSP=0x40 MEMORY=0x80 VI=0x100 PE_TOKEN=0x200 PE_FINISH=0x400
//     CP=0x800 DEBUG=0x1000 HSP=0x2000 RST(status)=0x10000.
//
// GC OS side — reference/sms/src/dolphin/os/OSInterrupt.c:
//   * __OSDispatchInterrupt (L293-412) reads intsr=__PIRegs[0] (0xCC003000), strips
//     bit16 (RSWST status, L304), bails if intsr==0 or (intsr & __PIRegs[1])==0 —
//     so PI mask @ 0xCC003004 = __PIRegs[1] (L306). native_dispatch_one in
//     dolphin_hook.cpp is already a faithful port of this; it reads
//     pi.GetCause()/GetMask() from Dolphin today.
//   * __OSMaskInterrupts/__OSUnmaskInterrupts (L253-291) program __PIRegs[1] for
//     PI-class sources — so the guest DOES write INTMR.
//
// ── Design (own STORAGE + the pending decision; leave device raises) ─────────
// READS of INTSR/INTMR are served from our OWN mirror — translation-independent,
// so a guest poll while transiently in real mode (mid-rfi, MSR.DR=0) can't miss.
// WRITES are SHADOWED, NOT diverted: we record intent into the mirror AND return
// false so memory_bridge still performs the real Dolphin store (Dolphin remains
// the authoritative raise/EE engine — its UpdateException() must keep firing).
// The public cause = our natively-latched bits ∪ Dolphin's live cause, so device
// raises that still go through Dolphin (CP/PE/AID) are seen without re-rooting them.
//
// ════════════════════════════════════════════════════════════════════════════
// ==== REQUIRED SEAM (apply manually — do NOT let me edit the file) ====
//   File: runtime/memory_bridge.cpp
//
//   (A) In mem_r32_slow(u32 ea), at the very TOP of the function (before any other
//       work), add:
//
//           extern "C" bool sb_pi_intercept_read(uint32_t ea, int bits, uint32_t* out);
//           { uint32_t pv; if (sb_pi_intercept_read(ea, 32, &pv)) return pv; }
//
//   (B) In mem_w32_slow(u32 ea, u32 v) AND mem_w16_slow(u32 ea, u16 v): immediately
//       after the existing MMIO_W(...) store completes (so the shadow stays in lock-
//       step with Dolphin's write), add:
//
//           extern "C" bool sb_pi_intercept_write(uint32_t ea, int bits, uint32_t val);
//           sb_pi_intercept_write(ea, 32, v);   // (use 16 + (u32)v in mem_w16_slow)
//
//   Both seam functions return false / do nothing when SUNBRIGHT_NATIVE_PI is unset,
//   so applying the seam is a no-op for the default build. The interceptor only
//   matters for the 32-bit cause/mask registers; the 16-bit write seam exists only
//   to capture any halfword INTMR programming (GC OS writes it as 32-bit, but the
//   seam is cheap and exact). NOTE: this file LINKS without the seam applied — the
//   seam functions are DEFINED here; the seam is just the call site that invokes them.
// ════════════════════════════════════════════════════════════════════════════
//
// Probe (optional): a /pi endpoint can be added to probe_server.cpp by calling
//   extern "C" void sb_native_pi_probe(char*, int, int*);
// (documented as a SECONDARY OPTIONAL seam at the bottom). Not required to run.
//
// ── How to verify ────────────────────────────────────────────────────────────
//  Build (NEW file → reconfigure first; CMake file(GLOB)):
//      cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_VULKAN=ON
//      cmake --build build -j$(nproc)
//  Apply the seam (A)+(B). A/B headless, each ~120 s:
//      SUNBRIGHT_HEADLESS=1 SUNBRIGHT_PROBE=1 ./build/sunbright &
//      SUNBRIGHT_HEADLESS=1 SUNBRIGHT_PROBE=1 SUNBRIGHT_NATIVE_PI=1 ./build/sunbright &
//  Healthy = both reach gameplay, audio plays (SUNBRIGHT_DUMP_AUDIO=1 →
//  scratch/wav/native_dsp.wav, sustained RMS>0), and /nintr per-source dispatch
//  counts climb at the same cadence A vs B (VI cause 0x100, DSP/AI, CP, PE_TOKEN).
//  No watchdog freeze, no new wild read/write. SUNBRIGHT_DBG_PI=1 logs the first 64
//  INTSR/INTMR accesses. If anything regresses, drop the flag (default OFF).
// ─────────────────────────────────────────────────────────────────────────────

#include "../overrides.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

#ifdef HAVE_DOLPHIN_CORE
#include "Core/System.h"
#include "Core/HW/ProcessorInterface.h"
#endif

namespace {

constexpr uint32_t PI_PHYS_CAUSE = 0x0C003000u;   // __PIRegs[0]
constexpr uint32_t PI_PHYS_MASK  = 0x0C003004u;   // __PIRegs[1]
constexpr uint32_t RSWST_STATUS  = 0x00010000u;   // bit16 stripped by __OSDispatchInterrupt

bool g_active = false;   // SUNBRIGHT_NATIVE_PI=1
bool g_dbg    = false;   // SUNBRIGHT_DBG_PI=1

std::atomic<uint32_t> g_pi_mask{0};
std::atomic<uint32_t> g_pi_cause_native{0};       // bits we latch ourselves

std::atomic<uint64_t> g_reads{0}, g_writes{0}, g_acks{0}, g_raises{0}, g_mask_writes{0};

bool init_flags() {
    static const bool once = [] {
        g_active = getenv("SUNBRIGHT_NATIVE_PI") != nullptr;
        g_dbg    = getenv("SUNBRIGHT_DBG_PI") != nullptr;
        // Never own PI under the pure-Dolphin oracle: it must stay bit-for-bit Dolphin.
        if (getenv("SUNBRIGHT_DISABLE_RECOMP")) g_active = false;
        if (g_active) fprintf(stderr, "[npi] native PI enabled (INTSR/INTMR owned)\n");
        return true;
    }();
    (void)once;
    return g_active;
}

uint32_t dolphin_cause() {
#ifdef HAVE_DOLPHIN_CORE
    return Core::System::GetInstance().GetProcessorInterface().GetCause();
#else
    return 0;
#endif
}

void dolphin_ack(uint32_t bits) {
#ifdef HAVE_DOLPHIN_CORE
    if (!bits) return;
    auto& pi = Core::System::GetInstance().GetProcessorInterface();
    for (uint32_t b = 1; b; b <<= 1)
        if (bits & b) pi.SetInterrupt(b, false);   // documented clear path; runs UpdateException()
#else
    (void)bits;
#endif
}

}  // namespace

// ── Public API consulted by the native dispatcher / device owners ────────────
extern "C" bool sb_native_pi_active() { return init_flags(); }

extern "C" uint32_t sb_native_pi_cause() {
    return g_pi_cause_native.load(std::memory_order_relaxed) | dolphin_cause();
}

extern "C" uint32_t sb_native_pi_mask() {
    return g_pi_mask.load(std::memory_order_relaxed);
}

extern "C" bool sb_native_pi_pending() {
    const uint32_t c = sb_native_pi_cause() & ~RSWST_STATUS;
    return c != 0 && (c & sb_native_pi_mask()) != 0;
}

extern "C" void sb_native_pi_ack(uint32_t bits) {
    if (!bits) return;
    g_acks.fetch_add(1, std::memory_order_relaxed);
    g_pi_cause_native.fetch_and(~bits, std::memory_order_relaxed);
    dolphin_ack(bits);
}

extern "C" void sb_native_pi_raise(uint32_t bit) {
    g_raises.fetch_add(1, std::memory_order_relaxed);
    g_pi_cause_native.fetch_or(bit, std::memory_order_relaxed);
}

// ── MMIO interception seam (called from memory_bridge.cpp slow paths) ─────────
// Both return false (no-op) when the flag is OFF.
extern "C" bool sb_pi_intercept_read(uint32_t ea, int bits, uint32_t* out) {
    if (!init_flags()) return false;
    if (bits != 32 || !out) return false;
    const uint32_t lo = ea & 0x0FFFFFFFu;
    if (lo == PI_PHYS_CAUSE) {
        *out = sb_native_pi_cause();
        g_reads.fetch_add(1, std::memory_order_relaxed);
        if (g_dbg) { static long n = 0; if (n++ < 64)
            fprintf(stderr, "[npi] R INTSR -> %08x (mask=%08x)\n", *out, sb_native_pi_mask()); }
        return true;
    }
    if (lo == PI_PHYS_MASK) {
        *out = sb_native_pi_mask();
        g_reads.fetch_add(1, std::memory_order_relaxed);
        if (g_dbg) { static long n = 0; if (n++ < 64)
            fprintf(stderr, "[npi] R INTMR -> %08x\n", *out); }
        return true;
    }
    return false;
}

// SHADOW the write into our mirror and ALWAYS return false so memory_bridge still
// performs the real Dolphin MMIO store (Dolphin must remain the raise/EE engine).
extern "C" bool sb_pi_intercept_write(uint32_t ea, int bits, uint32_t val) {
    if (!init_flags()) return false;
    if (bits != 32 && bits != 16) return false;
    const uint32_t lo = ea & 0x0FFFFFFCu;          // align to register word (16-bit halves alias)
    if (lo == PI_PHYS_CAUSE) {                      // write-1-to-clear
        g_writes.fetch_add(1, std::memory_order_relaxed);
        g_acks.fetch_add(1, std::memory_order_relaxed);
        g_pi_cause_native.fetch_and(~val, std::memory_order_relaxed);
        if (g_dbg) { static long n = 0; if (n++ < 64)
            fprintf(stderr, "[npi] W INTSR clear %08x (shadow, passthrough)\n", val); }
        return false;
    }
    if (lo == PI_PHYS_MASK) {                       // plain store
        g_pi_mask.store(val, std::memory_order_relaxed);
        g_mask_writes.fetch_add(1, std::memory_order_relaxed);
        if (g_dbg) { static long n = 0; if (n++ < 64)
            fprintf(stderr, "[npi] W INTMR <- %08x (shadow, passthrough)\n", val); }
        return false;
    }
    return false;
}

// ── /pi probe backing (SECONDARY OPTIONAL seam in probe_server.cpp) ──────────
extern "C" void sb_native_pi_probe(char* buf, int cap, int* used) {
    int n = snprintf(buf, (size_t)cap,
        "native_pi=%d cause=%08x (native=%08x dolphin=%08x) mask=%08x pending=%d\n"
        "reads=%llu writes=%llu mask_writes=%llu acks=%llu raises=%llu\n",
        (int)init_flags(), sb_native_pi_cause(),
        g_pi_cause_native.load(std::memory_order_relaxed), dolphin_cause(),
        sb_native_pi_mask(), (int)sb_native_pi_pending(),
        (unsigned long long)g_reads.load(), (unsigned long long)g_writes.load(),
        (unsigned long long)g_mask_writes.load(), (unsigned long long)g_acks.load(),
        (unsigned long long)g_raises.load());
    if (used) *used = (n < 0) ? 0 : (n > cap ? cap : n);
}
