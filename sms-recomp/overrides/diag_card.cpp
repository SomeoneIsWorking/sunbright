// diag_card.cpp — report what the guest's own CARD entry points return.
//
// The card mount fails somewhere between "device detected" and "directory read", and reasoning
// about which layer is responsible has already produced two wrong answers. The guest's return
// codes say it directly: each override runs the real body and reports its result.
//
// Diagnostic only, gated on SBR_CARD_TRACE. The real bodies always run.

#include "overrides.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdlib>

extern "C" void func_803580a8(CPUState&);   // CARDProbeEx(chan, *size, *sectorSize)
extern "C" void func_803588dc(CPUState&);   // CARDMount(chan, workArea, detachCallback)
extern "C" void func_80357f88(CPUState&);   // CARDCheck(chan)
extern "C" void func_8036a4cc(CPUState&);   // EXIProbeEx(chan)
extern "C" void func_8036b050(CPUState&);   // EXIGetID(chan, dev, *id)
extern "C" void func_8036a2d8(CPUState&);   // EXIProbe(chan)
extern "C" void func_8035873c(CPUState&);   // CARDMountAsync(chan, workArea, detach, cb)
extern "C" void func_803554e0(CPUState&);   // __CARDSync(chan)
extern "C" void func_8036a580(CPUState&);   // EXIAttach(chan, callback)

namespace {

bool tracing() {
    static const bool on = std::getenv("SBR_CARD_TRACE") != nullptr;
    return on;
}

// Report the first few results per entry point: these are called in a retry loop, so an
// unbounded log would bury the first failure that matters.
// Report only when a result CHANGES. A per-call cap hides exactly the event that matters here
// — these are polled in a retry loop, so the first N calls are all the same failure and a later
// success scrolls past the limit unseen. (That cap already misled this investigation once.)
void report(const char* what, s32 result) {
    static const char* names[8] = {};
    static s32 last[8] = {};
    static long runs[8] = {};
    int slot = 0;
    for (; slot < 8; ++slot) {
        if (names[slot] == nullptr) {
            names[slot] = what; last[slot] = result; runs[slot] = 1;
            lucent::info("card", "{} -> {}", what, result);
            return;
        }
        if (names[slot] == what) break;
    }
    if (slot >= 8) return;
    if (result == last[slot]) { ++runs[slot]; return; }
    lucent::info("card", "{} -> {} (after {} x {})", what, result, runs[slot], last[slot]);
    last[slot] = result; runs[slot] = 1;
}

#define TRACE_CARD(fn, addr, label)                                                            \
    void fn(CPUState& cpu) {                                                                   \
        func_##addr(cpu);                                                                      \
        if (tracing()) report(label, (s32)cpu.gpr[3]);                                          \
    }

TRACE_CARD(card_probe_ex, 803580a8, "CARDProbeEx")
TRACE_CARD(card_mount,    803588dc, "CARDMount")
TRACE_CARD(card_check,    80357f88, "CARDCheck")
TRACE_CARD(exi_probe_ex,  8036a4cc, "EXIProbeEx")
TRACE_CARD(exi_get_id,    8036b050, "EXIGetID")
// CARDMountAsync's first gate is a flag byte in OS low memory (0x800030e3 bit 0x80): set
// means "memory cards are disabled" and it returns NOCARD before touching hardware.
void card_mount_async(CPUState& cpu) {
    const u8 flag = sb_r8(0x800030e3u);
    func_8035873c(cpu);
    if (tracing()) {
        report("CARDMountAsync", (s32)cpu.gpr[3]);
        static bool once = false;
        if (!once) { once = true;
            lucent::info("card", "  low-mem card-disable byte 0x800030e3 = 0x{:02x}", flag); }
    }
}
TRACE_CARD(card_sync,        803554e0, "__CARDSync")
TRACE_CARD(exi_attach,       8036a580, "EXIAttach")

// EXIProbe's insertion debounce: it stores a 100ms-unit timestamp per channel at
// 0x800030c0 + chan*4 and requires 3 units to elapse. Report the raw inputs so the stall is
// visible as numbers rather than inferred from a return code.
void exi_probe(CPUState& cpu) {
    const u32 ch = cpu.gpr[3];
    func_8036a2d8(cpu);
    if (!tracing()) return;
    // Transitions only: this is polled thousands of times, and a fixed cap hides the change
    // that matters (it already did, three times).
    static s32 last = 0x7fffffff;
    static long run = 0;
    const s32 r = (s32)cpu.gpr[3];
    if (r == last) { ++run; return; }
    const u32 csr   = sb_r32(0xCC006800u + ch * 0x14u);
    const u32 stamp = sb_r32(0x800030c0u + ch * 4u);
    lucent::info("card", "EXIProbe(ch{}) -> {} after {} x {}  csr=0x{:08x} stamp={}", ch, r,
                 run, last, csr, stamp);
    last = r; run = 1;
}

} // namespace

SB_OVERRIDE(0x803580a8u, card_probe_ex, "CARDProbeEx (trace)", "diagnostic; real body runs")
SB_OVERRIDE(0x803588dcu, card_mount,    "CARDMount (trace)",   "diagnostic; real body runs")
SB_OVERRIDE(0x80357f88u, card_check,    "CARDCheck (trace)",   "diagnostic; real body runs")
SB_OVERRIDE(0x8036a4ccu, exi_probe_ex,  "EXIProbeEx (trace)",  "diagnostic; real body runs")
SB_OVERRIDE(0x8036b050u, exi_get_id,    "EXIGetID (trace)",    "diagnostic; real body runs")
SB_OVERRIDE(0x8036a2d8u, exi_probe,     "EXIProbe (trace)",    "diagnostic; real body runs")
SB_OVERRIDE(0x8035873cu, card_mount_async, "CARDMountAsync (trace)", "diagnostic; real body runs")
SB_OVERRIDE(0x803554e0u, card_sync,        "__CARDSync (trace)",     "diagnostic; real body runs")
SB_OVERRIDE(0x8036a580u, exi_attach,       "EXIAttach (trace)",      "diagnostic; real body runs")
