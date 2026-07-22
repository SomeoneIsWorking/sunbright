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

namespace {

bool tracing() {
    static const bool on = std::getenv("SBR_CARD_TRACE") != nullptr;
    return on;
}

// Report the first few results per entry point: these are called in a retry loop, so an
// unbounded log would bury the first failure that matters.
void report(const char* what, s32 result, int limit = 6) {
    static int counts[8] = {};
    static const char* names[8] = {};
    int slot = 0;
    for (; slot < 8; ++slot) {
        if (names[slot] == nullptr) { names[slot] = what; break; }
        if (names[slot] == what) break;
    }
    if (slot < 8 && counts[slot] < limit) {
        ++counts[slot];
        lucent::info("card", "{} -> {}", what, result);
    }
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

} // namespace

SB_OVERRIDE(0x803580a8u, card_probe_ex, "CARDProbeEx (trace)", "diagnostic; real body runs")
SB_OVERRIDE(0x803588dcu, card_mount,    "CARDMount (trace)",   "diagnostic; real body runs")
SB_OVERRIDE(0x80357f88u, card_check,    "CARDCheck (trace)",   "diagnostic; real body runs")
SB_OVERRIDE(0x8036a4ccu, exi_probe_ex,  "EXIProbeEx (trace)",  "diagnostic; real body runs")
SB_OVERRIDE(0x8036b050u, exi_get_id,    "EXIGetID (trace)",    "diagnostic; real body runs")
