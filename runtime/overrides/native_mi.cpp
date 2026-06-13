// Native Memory Interface (MI) — PC-native ownership of the GameCube MI register block
// (physical 0xCC004000-0xCC004040) and the GC OS memory-config / protection layer that
// drives it. Opt-in via SUNBRIGHT_NATIVE_MI=1; default OFF leaves everything on the
// existing Dolphin-MMIO + recomp path (unchanged behavior).
//
// ─────────────────────────────────────────────────────────────────────────────────────
// WHAT THE MI IS (reverse-engineered from reference/sms/src/dolphin/os/OSMemory.c and
// externals/dolphin/Source/Core/Core/HW/MemoryInterface.cpp)
//
// The MI is the Flipper memory-controller register file. It exposes:
//   • 4 protection REGIONS (per-channel first_page/last_page, 1 KB page granularity)
//   • a PROT_TYPE register (per-channel R/W/RDWR/NONE bits)
//   • an IRQ mask + IRQ flag for the memory-protection-fault interrupt
//   • a captured fault ADDRESS (prot_addr hi/lo) for the handler to read
//   • 10 refresh/perf TIMER counters (unused by SMS gameplay)
//
// Register layout (16-bit registers, byte offsets from MI base 0xCC004000):
//   0x00..0x0E  region0..3 first/last page   (MI_REGION{0..3}_{FIRST,LAST})
//   0x10        prot_type                    (MI_PROT_TYPE,  __MEMRegs[8])
//   0x1C        irq_mask                     (MI_IRQMASK)
//   0x1E        irq_flag                     (MI_IRQFLAG,    __MEMRegs[15])
//   0x20        unknown1                     (__MEMRegs[16]) — OS writes 0 at init
//   0x22        prot_addr.hi                 (MI_PROT_ADDR_LO in Dolphin naming)
//   0x24        prot_addr.lo                 (MI_PROT_ADDR_HI in Dolphin naming)
//   0x28..0x40  timers 0..9 (hi/lo pairs)
// __MEMRegs (the OS view) is the *16-bit-indexed* alias of this block:
//   __MEMRegs[n]  == 16-bit register at byte offset 2*n.
//   __MEMRegs[8]  -> 0x10 (prot_type), __MEMRegs[15] -> 0x1E (irq_flag),
//   __MEMRegs[16] -> 0x20 (unknown1),  __MEMRegs[20] -> 0x28 (timer0_hi).
//
// HOW SMS USES IT (OSMemory.c):
//   __OSInitMemoryProtection (0x803465b8):
//       • runs Config24MB/Config48MB in REAL MODE to program the main-RAM BATs
//         (already owned natively as a no-op: runtime/overrides/sms_os_memprotect.cpp,
//          the 0x803465a0 trampoline — flat memory model needs no BATs);
//       • __MEMRegs[16]=0; __MEMRegs[8]=0xFF  (prot_type RDWR on all 4 channels);
//       • masks the 4 MEM protection IRQs + registers MEMIntrruptHandler;
//       • if simulatedSize < physicalSize && simulatedSize==24MB: __MEMRegs[20]=2;
//       • unmasks the MEM_ADDRESS IRQ.
//   OSProtectRange (inlined in SMS — no standalone entry): writes a region's
//       first/last page + adjusts prot_type; enables the fault IRQ when control != RDWR.
//   MEMIntrruptHandler: on a protection-fault IRQ reads cause(__MEMRegs[15]) +
//       addr(prot_addr), clears it, and calls the registered error handler.
//
// WHY THIS CAN WEDGE / MISBEHAVE UNDER THE HYBRID (same class as the DSP 0xCC00500A read)
//   1. These register accesses occur around OSDisableInterrupts/OSRestoreInterrupts and,
//      for the init path, partly in REAL MODE (MSR.DR=0). A 16-bit MMIO read taken while
//      the guest is transiently real-mode misses Dolphin's translate path the same way the
//      DSP-status read did — it returns garbage, and the OS then programs the protection
//      unit from a garbage prot_type. (Reads ARE rerouted to the physical MMIO mapping by
//      memory_bridge's mmio_r, which mitigates this for reads; MI was simply never special-
//      cased, so it depended entirely on Dolphin's MemoryInterfaceManager being live.)
//   2. The memory-protection-fault interrupt is a real PI interrupt source. Under the
//      time-parked governor a CoreTiming-scheduled MI IRQ would never fire (the
//      "deliver-later event never fires" class behind the CP wedge); on hardware SMS never
//      triggers a protection fault in normal play (it sets RDWR everywhere), but a stray
//      enabled fault IRQ could leave PI with a pending source nothing services.
//
// THE PORT (own it). A native 32-entry u16 mirror of 0xCC004000-0xCC004040 IS the MI for
// our runtime. We have no MMU and a flat 24 MB main-RAM mapping, so memory PROTECTION has
// no observable effect on guest-visible state (there are no host page permissions backing
// guest RAM, and the game never intentionally faults). Therefore:
//   • READS of any MI register return the native-mirror value — translation-independent,
//     correct regardless of MSR.DR, never "Unable to resolve".
//   • WRITES record into the mirror (so a subsequent read-back is consistent — OSProtectRange
//     reads __MEMRegs[8] back to OR in the new control bits) but are otherwise harmless:
//     programming a protection region over a flat, unprotected mapping is a no-op, and we
//     NEVER raise a protection-fault interrupt (so there is no MI-IRQ wedge — the protection
//     unit is inert by construction, exactly as it effectively is for SMS, which only ever
//     sets RDWR).
//   • __OSInitMemoryProtection (0x803465b8) is replaced by a faithful native equivalent:
//     prime the mirror (prot_type=0xFF RDWR-all, unknown1=0, the __MEMRegs[20]=2 24MB case),
//     leave the IRQ flag/mask clear (no fault ever delivered), and return. The real-mode BAT
//     hop is already a no-op (sms_os_memprotect.cpp); the IRQ-handler registration is
//     unnecessary because no MI fault is ever raised.
//
// OS MEMORY-CONFIG GETTERS — IMPORTANT RE FINDING:
//   OSGetPhysicalMemSize / OSGetConsoleSimulatedMemSize are NOT standalone functions in
//   the SMS GMSE01 DOL — the compiler INLINED them at every call site (a disasm scan of the
//   OS .text found no `lwz r3,0x28(r0); blr` getter body; the only such loads are inside
//   __OSInitMemoryProtection itself). They read the globals:
//       __OSPhysicalMemSize   @ 0x80000028   (physical MEM1 size, 24 MB)
//       __OSSimulatedMemSize  @ 0x800000F0   (simulated mem size, 24 MB)
//   Dolphin's BS2 emu already writes both at boot (Boot_BS2Emu.cpp:255/486 ->
//   GetRamSizeReal() = 0x1800000). So there is no getter address to override; instead we
//   DEFENSIVELY pin both globals to 24 MB when NATIVE_MI is on (idempotent with Dolphin),
//   so the inlined reads return the correct console memory size even if some earlier path
//   left them zero. This makes the memsize answer independent of Dolphin's MI/BS2 state.
//
// ─────────────────────────────────────────────────────────────────────────────────────
// REQUIRED SEAM (apply manually — memory_bridge.cpp is NOT edited by this file).
//
// To route MI register reads/writes to the native mirror, add the MI fast-path to the MMIO
// read/write dispatch in runtime/memory_bridge.cpp. Inside `mmio_r<T>` (template), BEFORE
// the `is_mmio_phys` branch, and inside the MMIO_W macro path, add:
//
//   // ==== REQUIRED SEAM (apply manually) ==== native MI mirror (SUNBRIGHT_NATIVE_MI)
//   extern bool sb_native_mi_enabled();
//   extern bool sb_native_mi_read (u32 phys, int bytes, u32* out);   // true if served
//   extern bool sb_native_mi_write(u32 phys, int bytes, u32 val);    // true if served
//
//   template <typename T> static inline T mmio_r(u32 ea) {
//       const u32 phys = ea & 0x0FFFFFFFu;
//       if (sb_native_mi_enabled() && phys >= 0x0C004000u && phys <= 0x0C004040u) {
//           u32 out = 0;
//           if (sb_native_mi_read(phys, (int)sizeof(T), &out)) return (T)out;
//       }
//       if constexpr (sizeof(T) <= 4) {
//           if (is_mmio_phys(ea))
//               return MEM().GetMMIOMapping()->Read<T>(Core::System::GetInstance(), phys);
//       }
//       return MMU_().Read<T>(ea);
//   }
//
// And replace the MMIO_W macro body to consult the MI mirror first:
//   #define MMIO_W(bits, ea, v)  (g_recomp_touched_mmio = true,                          \
//       (sb_native_mi_enabled() &&                                                        \
//        ((ea) & 0x0FFFFFFFu) >= 0x0C004000u && ((ea) & 0x0FFFFFFFu) <= 0x0C004040u &&   \
//        sb_native_mi_write((ea) & 0x0FFFFFFFu, (bits)/8, (v)))                           \
//       ? (void)0 : (void)MMU_().Write<u##bits>((v), (ea)))
//
// The seam is OPTIONAL for the OS-function override below to work: with NATIVE_MI on, the
// __OSInitMemoryProtection override never touches Dolphin's MI at all (it writes the native
// mirror directly), so even WITHOUT the seam, boot does not depend on Dolphin's MI for the
// init path. The seam is what additionally makes any *recompiled* OSProtectRange / handler
// register reads resolve from the mirror; without it those still fall to Dolphin's MI, which
// is harmless (Dolphin's mirror is also live) but not "owned".
//
// ─────────────────────────────────────────────────────────────────────────────────────
// VERIFY RECIPE
//   1. Build: a NEW file needs a reconfigure (CMake file(GLOB)):
//        cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_VULKAN=ON && cmake --build build -j
//   2. Boot headless with the flag on and confirm it reaches gameplay (no MI wedge, no
//      regression vs default-off):
//        SUNBRIGHT_HEADLESS=1 SUNBRIGHT_NATIVE_MI=1 SUNBRIGHT_FASTBOOT=1 \
//          SUNBRIGHT_DBG_MI=1 ./build/sunbright   # watch for "[native_mi] init"
//   3. Memsize reports 24 MB: probe the globals via the REPL —
//        curl 'http://127.0.0.1:17654/r?a=80000028&n=1'   # expect 0x01800000 (physical)
//        curl 'http://127.0.0.1:17654/r?a=800000f0&n=1'   # expect 0x01800000 (simulated)
//      and the MI prot_type mirror after init — curl '/r?a=cc004010&n=1' is served by Dolphin;
//      with the seam, [native_mi] log shows prot_type=0x00ff.
//   4. A/B: same run with SUNBRIGHT_NATIVE_MI unset must reach the same point (default OFF
//      changes nothing). No new wild-read/wild-write traps in either.
// ─────────────────────────────────────────────────────────────────────────────────────

#include "../overrides.h"
#include "../intrinsics.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

namespace {

// MI base (physical) and the register-block extent we own.
constexpr u32 MI_PHYS_BASE = 0x0C004000u;
constexpr u32 MI_PHYS_LAST = 0x0C004040u;     // inclusive last byte offset register (timer9_lo)
constexpr u32 MI_SIZE      = 0x44u;           // bytes covered (0x00..0x40 inclusive + 16-bit reg)

// OS memory-config globals (read inline by SMS; no standalone getter functions exist).
constexpr u32 ADDR_PHYSICAL_MEMSIZE  = 0x80000028u;   // __OSPhysicalMemSize
constexpr u32 ADDR_SIMULATED_MEMSIZE = 0x800000F0u;   // __OSSimulatedMemSize
constexpr u32 GC_MEM1_SIZE_24MB      = 0x01800000u;   // 24 MB retail MEM1

// __OSInitMemoryProtection entry (GMSE01). The real-mode BAT hop (0x803465a0) is owned
// separately as a no-op (sms_os_memprotect.cpp); here we own the whole init function.
constexpr u32 INIT_MEMORY_PROTECTION = 0x803465b8u;

// __MEMRegs 16-bit register byte offsets (within the block).
constexpr u32 MI_OFF_PROT_TYPE = 0x10;   // __MEMRegs[8]
constexpr u32 MI_OFF_IRQMASK   = 0x1C;
constexpr u32 MI_OFF_IRQFLAG   = 0x1E;   // __MEMRegs[15]
constexpr u32 MI_OFF_UNKNOWN1  = 0x20;   // __MEMRegs[16]
constexpr u32 MI_OFF_REG20     = 0x28;   // __MEMRegs[20] (timer0_hi byte offset)

// The native MI mirror: 0x44 bytes covering registers at 0x00..0x40. Indexed by the byte
// offset from MI_PHYS_BASE. All registers are 16-bit; we store big-endian-neutral host u16s
// keyed by even offset, and serve byte/halfword/word reads from this array. Stored in the
// "OS view" (native host endianness) so the mirror is trivial to reason about; the seam
// returns host-order values exactly as Dolphin's DirectRead<u16> would (the recomp/MMU layer
// applies the same conversion either way).
u16  g_mi[MI_SIZE / 2];   // 34 u16 registers
bool g_native_mi = false;
bool g_inited    = false;
bool g_dbg       = false;

inline u16& reg16(u32 off) { return g_mi[(off & 0x3Eu) / 2]; }

void mi_log(const char* what) {
    if (!g_dbg) return;
    fprintf(stderr, "[native_mi] %s prot_type=0x%04x irqmask=0x%04x irqflag=0x%04x "
                    "unknown1=0x%04x reg20=0x%04x\n",
            what, reg16(MI_OFF_PROT_TYPE), reg16(MI_OFF_IRQMASK), reg16(MI_OFF_IRQFLAG),
            reg16(MI_OFF_UNKNOWN1), reg16(MI_OFF_REG20));
}

// Pin the OS memsize globals to 24 MB (idempotent with Dolphin's BS2 emu). Done so the
// inlined OSGetPhysicalMemSize / OSGetConsoleSimulatedMemSize reads are correct independent
// of Dolphin's MI/BS2 state. Safe: writes guest RAM via the normal bridge.
void pin_memsize_globals() {
    if (mem_r32(ADDR_PHYSICAL_MEMSIZE)  != GC_MEM1_SIZE_24MB)
        mem_w32(ADDR_PHYSICAL_MEMSIZE,  GC_MEM1_SIZE_24MB);
    if (mem_r32(ADDR_SIMULATED_MEMSIZE) != GC_MEM1_SIZE_24MB)
        mem_w32(ADDR_SIMULATED_MEMSIZE, GC_MEM1_SIZE_24MB);
}

void native_mi_init_once() {
    if (g_inited) return;
    g_inited     = true;
    g_native_mi  = getenv("SUNBRIGHT_NATIVE_MI") != nullptr;
    g_dbg        = getenv("SUNBRIGHT_DBG_MI") != nullptr;
    std::memset(g_mi, 0, sizeof(g_mi));
    if (g_native_mi) mi_log("init (mirror zeroed)");
}

// Faithful native __OSInitMemoryProtection: program the MI mirror exactly as the guest would,
// minus the unmodelable real-mode BAT hop (already a no-op) and minus the protection-fault
// IRQ machinery (we never raise an MI fault, so masking/registering it is unnecessary).
//   __MEMRegs[16] = 0;            // unknown1 = 0
//   __MEMRegs[8]  = 0xFF;         // prot_type = RDWR on all 4 channels
//   (4 MEM IRQs masked; handler registered)  -> skipped: no MI fault ever delivered
//   if (simulated < physical && simulated == 24MB) __MEMRegs[20] = 2;
//   (MEM_ADDRESS IRQ unmasked)   -> skipped: no MI fault ever delivered
SUNBRIGHT_OVERRIDE(ov_init_memory_protection, INIT_MEMORY_PROTECTION) {
    native_mi_init_once();
    if (!g_native_mi) {
        // Flag OFF: behave as if no override exists — run the real recompiled body so default
        // behavior is byte-for-byte unchanged. recomp_raw() returns the original generated func.
        if (RecompFunc raw = recomp_raw(INIT_MEMORY_PROTECTION)) { raw(cpu); return; }
        // No recomp body (shouldn't happen): fall through to a harmless return.
        return;
    }

    pin_memsize_globals();

    reg16(MI_OFF_UNKNOWN1)  = 0x0000;
    reg16(MI_OFF_PROT_TYPE) = 0x00FF;            // __MEMRegs[8] = 0xFF (RDWR all channels)
    reg16(MI_OFF_IRQMASK)   = 0x0000;            // all MEM protection IRQs masked
    reg16(MI_OFF_IRQFLAG)   = 0x0000;            // no fault pending

    const u32 simulated = mem_r32(ADDR_SIMULATED_MEMSIZE);
    const u32 physical  = mem_r32(ADDR_PHYSICAL_MEMSIZE);
    if (simulated < physical && simulated == GC_MEM1_SIZE_24MB)
        reg16(MI_OFF_REG20) = 2;                 // __MEMRegs[20] = 2

    mi_log("__OSInitMemoryProtection (native)");
    // call_ppc continues inline at LR (this is a normal C-return function — the real-mode
    // hop inside is owned elsewhere; nothing else here needs the guest stack).
}

}  // namespace

// ── Seam entry points (consumed by the REQUIRED SEAM in memory_bridge.cpp) ───────────────
// Defined at global scope (extern "C++") so memory_bridge can forward-declare and call them
// without including this file. With NATIVE_MI off, sb_native_mi_enabled() is false and the
// seam is a no-op fast-path — Dolphin's MI serves everything (unchanged).
bool sb_native_mi_enabled() {
    native_mi_init_once();
    return g_native_mi;
}

// Serve a read of the MI block from the native mirror. phys is the PHYSICAL address
// (0x0C004xxx). bytes is 1/2/4. Returns true and fills *out if served; false to fall through.
bool sb_native_mi_read(u32 phys, int bytes, u32* out) {
    if (!g_native_mi) return false;
    if (phys < MI_PHYS_BASE || phys > MI_PHYS_LAST) return false;
    const u32 off = phys - MI_PHYS_BASE;
    switch (bytes) {
        case 2: {
            if (off + 2 > MI_SIZE) return false;
            *out = reg16(off);
            return true;
        }
        case 1: {
            if (off + 1 > MI_SIZE) return false;
            const u16 r = reg16(off & ~1u);
            *out = (off & 1u) ? (r & 0xFFu) : (r >> 8);   // big-endian byte select within reg
            return true;
        }
        case 4: {
            // 32-bit read of two adjacent 16-bit registers (hi at off, lo at off+2),
            // matching Dolphin's ReadToSmaller<u32> composition.
            if (off + 4 > MI_SIZE) return false;
            *out = ((u32)reg16(off) << 16) | reg16(off + 2);
            return true;
        }
        default: return false;
    }
}

// Record a write into the MI mirror. Protection programming has no host effect (flat,
// unprotected mapping); we keep the value so read-backs (OSProtectRange OR-ing prot_type) are
// consistent, and we NEVER set the IRQ flag from a write (no fault is ever raised).
bool sb_native_mi_write(u32 phys, int bytes, u32 val) {
    if (!g_native_mi) return false;
    if (phys < MI_PHYS_BASE || phys > MI_PHYS_LAST) return false;
    const u32 off = phys - MI_PHYS_BASE;
    switch (bytes) {
        case 2:
            if (off + 2 > MI_SIZE) return false;
            reg16(off) = (u16)val;
            break;
        case 1: {
            if (off + 1 > MI_SIZE) return false;
            u16& r = reg16(off & ~1u);
            if (off & 1u) r = (u16)((r & 0xFF00u) | (val & 0xFFu));
            else          r = (u16)((r & 0x00FFu) | ((val & 0xFFu) << 8));
            break;
        }
        case 4:
            if (off + 4 > MI_SIZE) return false;
            reg16(off)     = (u16)(val >> 16);
            reg16(off + 2) = (u16)(val & 0xFFFFu);
            break;
        default: return false;
    }
    if (g_dbg) {
        static unsigned long n = 0;
        if ((n++ & 0xFu) == 0)
            fprintf(stderr, "[native_mi] write off=0x%02x bytes=%d val=0x%x\n", off, bytes, val);
    }
    return true;
}
