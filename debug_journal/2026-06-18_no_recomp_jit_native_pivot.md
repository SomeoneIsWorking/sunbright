# 2026-06-18 — Architecture pivot: DELETE the static recompiler from the game (JIT-native engine)

USER DIRECTIVE (2026-06-18): "I don't want a recomp. The game engine should run as PC-native and
the gameplay should run in either JIT or interpreter." + "delete the recomp from the game itself"
(a standalone static-analysis tool like Ghidra is fine to keep — just no recomp executing in the
game). On interpreter: "if PC Engine + JIT will work fine then okay" / don't bother measuring interp
speed (ngx is already slow in Delfino so the measurement would mislead).

Target architecture: **PC-native engine (the existing overrides: ngx renderer, native_jas audio,
native_card, drawsync, nthr OS/threading) + gameplay logic under Dolphin's JIT. No static recomp.**

## Phase A — DONE & VERIFIED (this session)
Added `SUNBRIGHT_NO_RECOMP=1` dispatch in `IsRecompiled` (runtime/sunbright_bridge.cpp): check
overrides + native-OS + force_jit FIRST, then `if (no_recomp) return false` so all other gameplay
falls through to Dolphin's JIT. The recomp table is STILL linked in this phase so override
super-calls (`recomp_raw`) keep working.

VERIFIED: `SUNBRIGHT_NO_RECOMP=1 SUNBRIGHT_FASTBOOT=1` boots into Delfino Plaza and the **Dolphin-GX
output renders the scene correctly** (Mario, HUD coin/shine/water-gauge, NPCs, plaza, buildings —
screenshot scratch/screenshots/ab2.gx.png). So gameplay logic runs correctly under Dolphin JIT with
the native engine intercepting. ngx capture also works (frame_swaps advancing, 207k shapes/frame).

NOT the same as `SUNBRIGHT_DISABLE_RECOMP`, which returns false BEFORE the override/native-OS checks
(pure-Dolphin oracle, no native engine).

## Phase A caveat — recomp still dominates execution (the real work is Phase B)
Live in Delfino: **~2.16M recomp calls/sec** still (interp ~13.5k/s). So gameplay is NOT actually on
JIT yet — it renders via Dolphin GX, but the heavy execution is still recompiled code, pulled in by
override super-calls. Flow: Dolphin JIT runs the frame → calls an overridden fn → trampoline → Run →
override → `recomp_raw(addr)` super-call → recomp body → `call_ppc` to callees → `recomp_lookup`
hits → the whole subtree runs as recomp. The ngx capture overrides (observe + super-call the original
J3DShape/J2DScreen draw so Dolphin keeps rasterizing) are a big contributor — 16 `recomp_raw` sites.

## Phase B — the hard part (DESIGN, decisive constraint found)
**DECISIVE FACT: the Dolphin interpreter does NOT consult our override hooks — only the JIT
trampoline does.** Fork hook patches exist only in Jit64/JitArm64/JitCommon, none in
`Source/Core/Core/PowerPC/Interpreter/`. Consequences:
- Running a guest function under the **interpreter** (the existing `call_ppc` non-recomp path,
  `interp.SingleStep`) executes raw PPC and its sub-`bl`s WITHOUT hitting overrides → the native
  engine stops intercepting → broken. So the interpreter is NOT a valid substrate for super-calls
  (or gameplay) in no-recomp mode. This also kills the "interpreter might be fast enough" option on
  correctness grounds, not just speed.
- Running under **Dolphin JIT** DOES go through the trampoline at each block dispatch (block-linking
  is off), so overrides keep intercepting. JIT is the only valid substrate.

So Phase B must make override super-calls (and recomp→recomp `call_ppc`) run the original guest under
**Dolphin JIT and return**, instead of running the recomp body. That primitive ("run guest fn at
addr under Dolphin JIT, return when it blr's to ret, overrides still active") does not exist yet and
is non-trivial: we're already inside Dolphin's CPU loop (override entered via trampoline→Run), so it
needs reentrant JIT execution or a fork-level pre-hook/post-hook mechanism.

Candidate Phase-B approaches (pick after deciding):
1. **Fork pre-hook**: trampoline runs the observe-override, then returns FALSE so Dolphin JITs the
   ORIGINAL block; the original's sub-calls then re-consult overrides naturally. Works for
   observe-then-super-call overrides (most of ngx capture). Needs a post-hook variant for overrides
   that also do work AFTER the original returns.
2. **Run-under-JIT-and-return primitive**: a reentrant "Dolphin CPU loop until pc==ret at sp_floor"
   that overrides call in place of `recomp_raw`. Conceptually a JIT twin of `interp_run_until`.
3. Convert each wrapping override to a full native replacement (no super-call) where feasible —
   reduces the seam surface but is per-override RE work.

Then Phase C: stop linking generated/, delete tools/recompiler from the build + the recomp call
model, make JIT-native the default; keep offline static-analysis tooling.

## Files
- runtime/sunbright_bridge.cpp — `IsRecompiled` no_recomp branch (Phase A).
- Memory: no-recomp-jit-native-direction.
