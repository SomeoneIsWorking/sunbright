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
        // (REMOVED 2026-06-11) force_jit_range(0x80301c00,0x80301e00) — an old stopgap that
        // routed the JAI per-frame work (processFrameWork/checkStream) to the interpreter to
        // dodge a then-unexplained data-dependent corruption. Under the current architecture
        // it silently KILLED all sequenced audio: the interpreted frame work never matured SE
        // requests (the jingle-dead bug's true root). The corruption it dodged predates the
        // rlwinm/addc/dcbz/paired-single emitter fixes; if it resurfaces, root-cause the
        // emitter — never re-add the route (debug-path rule: force_jit is diagnostics only).

        // ── Native overrides ─────────────────────────────────────────────────
        // Register with SUNBRIGHT_OVERRIDE in a .cpp here for a hand-written
        // replacement (e.g. a native memcpy/memset or matrix routine).
    }
} g_sms_overrides;

}  // namespace
