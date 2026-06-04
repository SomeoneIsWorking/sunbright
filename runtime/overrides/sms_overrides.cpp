// Super Mario Sunshine–specific overrides and JIT-forced addresses.
//
// This is the hand-tuned counterpart to the auto-generated recompiler output:
// when the static recompiler can't faithfully reproduce a function (HW/MMIO side
// effects, computed branches, etc.) we either route it to Dolphin's JIT or supply
// a native replacement here. Entries are registered at launcher startup.

#include "../overrides.h"
#include "../intrinsics.h"

namespace {

struct SMSOverrides {
    SMSOverrides() {
        // Functions that touch hardware SPRs (HID0/HID2, L2CR, WPAR, BATs, …),
        // MSR, or the MMU are routed to Dolphin's JIT automatically by the
        // recompiler (see function_needs_jit() in tools/recompiler/main.cpp), so
        // no manual force_jit() is needed for those anymore.
        //
        // Use this file for game-specific exceptions the recompiler can't infer:
        //   force_jit(0x80XXXXXX);          // hand-route a stubborn function
        //   force_jit_range(lo, hi);        // …or a whole region
        //
        // ── SMS-specific JIT routing ─────────────────────────────────────────
        // JAIBasic audio per-frame work (processFrameWork/checkStream/
        // checkDummyPositionBuffer, 0x80301c3c..0x80301de8). Its recomp call tree
        // corrupts the JAIBasic singleton DATA-DEPENDENTLY → later
        // checkPlayingSeq (0x80307c44) dereferences a NULL `this` (ea=0x180) once
        // the level is up. force-JIT bisection: routing any function in this tree
        // clears it. Audio frame-work is not hot-path, so route the cluster to
        // Dolphin's JIT (documented hybrid) rather than chase the data-dependent
        // miscompile. The proper emitter fix is TODO; tuned for THIS game.
        force_jit_range(0x80301c00u, 0x80301e00u);

        // ── Native overrides ─────────────────────────────────────────────────
        // Register with SUNBRIGHT_OVERRIDE in a .cpp here for a hand-written
        // replacement (e.g. a native memcpy/memset or matrix routine).
    }
} g_sms_overrides;

}  // namespace
