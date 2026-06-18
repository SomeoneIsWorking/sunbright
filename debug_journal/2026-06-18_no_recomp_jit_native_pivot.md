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

## Phase B progress + census findings (2026-06-18)
- **ngx owns J3DShape::draw natively** (committed) — SUNBRIGHT_NGX_NATIVE_DRAW, auto under
  NO_RECOMP+NGX_PRESENT. Verified faithful (static geometry delta 0-2; only Mario band differs by
  animation phase). Cut only ~8% of recomp → the draw super-call was NOT the dominant consumer.
- **Where the ~2M recomp calls/s actually come from** (SUNBRIGHT_CALL_CENSUS=1, /census →
  scratch/logs/call_census.tsv, under NO_RECOMP+NGX_PRESENT in Delfino). Top call_ppc targets:
  checkDistance (2.0M), TViewObj::testPerform (1.5M), sinf (1.2M), PSMTXCopy (1.0M),
  TLiveActor::perform, OSGetTick, OSYieldThread, GXSetTev* cluster. These are the GAMEPLAY +
  SCENE-GRAPH + GX tree.
- **KEY MECHANISM FINDING:** `recomp_lookup` (used by `call_ppc`) and `recomp_raw` (super-calls) do
  NOT honor `no_recomp` — they still return the recomp body. So the moment ANY native override
  either super-calls OR `call_ppc`s into guest code, the whole recompiled subtree (scene graph,
  math, GX) runs as recomp. That is the 2M/s. The math overrides themselves are fully native (they
  only super-call under the SUNBRIGHT_MATH_SHADOW diagnostic); the recomp is reached transitively.
- So removing recomp is NOT a matter of editing 22 override files one by one — it needs the
  **central seam**: under no_recomp, `call_ppc`/super-call must run the original under Dolphin (so
  it keeps consulting overrides) instead of the recomp body. The ONLY override-preserving substrate
  is Dolphin JIT (the interpreter bypasses overrides — confirmed). 

## REVISED Phase B recommendation (after the deeper dig)
Approach #2 (reentrant JIT-run-until-return) is the risky/no-precedent path. Approach #1 (fork
pre/post-hook: trampoline runs the observe-hook, returns FALSE so Dolphin JITs the ORIGINAL block;
sub-calls then re-consult overrides) is Dolphin's OWN HLE pattern (run C, return to dispatcher) and
needs NO reentrancy. The census shows the wrapping overrides are mostly observe-then-super-call
(interp_redraw perform hooks [INTERP60-gated, off by default], ngx GX tees, scene_render projection)
— a good fit for pre-hooks; a post-hook variant covers the few that do after-work. Lean #1.

## ★ MAJOR FINDING (2026-06-18): Phase A "JIT gameplay" was PARTLY ILLUSORY
Traced the recomp root under NO_RECOMP via SUNBRIGHT_DBG_CPBT=<addr> (one-shot host backtrace in
call_ppc). The chain rooting the whole game tree:
```
JitTrampoline → Run → dbg_logo_creator/driver (dbg_logo.cpp, override on the TApplication boot/frame
   state machine func_802a6398/802a5f50) → [super-call recomp] → ov_interp_mardir_direct
   (interp_redraw.cpp, override on TMarDirector::direct 0x80299838) → [super-call recomp] →
   the ENTIRE scene-graph tree (TObjHitCheck → checkDistance, perform, draws) AS RECOMP.
```
So under NO_RECOMP the FRAME LOOP itself ran as RECOMP (via dbg_logo's unconditional super-call),
with Dolphin JIT only filling the gaps. The 2M recomp/s WAS the game running as recomp. "Gameplay
under JIT" was not actually happening.

These overrides are FEATURE/observer hooks registered UNCONDITIONALLY that only super-call when their
feature is off: dbg_logo (SUNBRIGHT_DBG_LOGO, off), all interp_redraw + interp_capture overrides
(INTERP60, off — incl. a DUPLICATE override on J3DShape::draw 0x802e0390 that conflicted with ngx's).
FIX: SUNBRIGHT_OVERRIDE_IF macro — register only when the feature env is set. Gated dbg_logo on
DBG_LOGO, interp_* on INTERP60/REPLAY.

RESULT: default recomp path still boots+renders (fs advancing) — gating is SAFE. But NO_RECOMP now
**HANGS** (4.7M VI fields, pace=0ms, frame_swaps=0, recomp ~0): with the frame-loop super-call gone,
the app loop must run under Dolphin JIT and DOES NOT PROGRESS. So the REAL Phase-B blocker is exposed:
**the game does not actually run under Dolphin JIT in NO_RECOMP mode** — it spins very early. Likely
the native_os (nthr) scheduler ↔ Dolphin-JIT thread-driving interaction (pure DISABLE_RECOMP boots,
but that has NO native_os either). NEXT: debug why Dolphin-JIT + native_os hangs the boot/frame loop
under NO_RECOMP (compare DISABLE_RECOMP which works; the delta is overrides + native_os). This is the
true "make gameplay run under JIT" task — everything else (deleting recomp) depends on it.

## ★ BLOCKER NARROWED (2026-06-18): NO_RECOMP hangs at BOOT — engine-overrides ↔ Dolphin-JIT
After gating the feature overrides, NO_RECOMP hangs at the very start (millions of VI fields,
pace=0ms, frame_swaps=0, recomp ~0). Narrowed:
- NOT native_os: gating native_os_lookup off under no_recomp (let Dolphin own threading) did NOT
  fix the hang (reverted that experiment).
- NOT fastboot: NO_RECOMP + AUTOSTART (no fastboot) hangs identically.
- Pure SUNBRIGHT_DISABLE_RECOMP (no overrides, no native_os) BOOTS. NO_RECOMP (native ENGINE
  overrides active + Dolphin JIT) HANGS. → The cause is the **native engine overrides interacting
  with a pure Dolphin-JIT boot**. Those overrides (audio, GX FIFO/drawsync, memory bridge, GX init,
  etc.) were ALL built/tuned for the RECOMP HYBRID (recomp call model + native_os scheduler +
  specific charge_guest_time/CoreTiming pacing). They have never run on top of a pure Dolphin-JIT
  boot and several evidently break/spin it.

CONFIRMED (2026-06-18): SUNBRIGHT_NORECOMP_NOOV=1 (skip ALL overrides + native_os under no_recomp)
→ the boot NO LONGER spins (log shows normal asset loading: data/nintendo.szs, zelda ucode — not the
[vi-perf] field flood). frame_swaps stays 0 only because ngx capture + FASTBOOT are themselves
overrides (skipped under NOOV). So the engine overrides ARE the cause, proven. Next = per-GROUP
bisection.

NEXT (fresh session): BISECT which override group hangs the Dolphin-JIT boot. Method: under
SUNBRIGHT_NO_RECOMP, selectively disable override groups (the registration is now gateable via
SUNBRIGHT_OVERRIDE_IF; add a group kill-switch env, or temporarily skip override_lookup in
IsRecompiled for ranges) until boot progresses, then re-add to pin the offender(s). Likely suspects:
the GX FIFO / drawsync natives (sms_drawsync_lossproof, gx_stream) and the audio/AID natives, which
assume the recomp pacing/poll_yield. STRATEGIC QUESTION to resolve: whether "Dolphin-JIT + existing
hybrid-era engine overrides" is viable, or whether the full-PC-engine end state needs the engine
overrides re-grounded for a JIT (non-recomp) host. Compare against the working DISABLE_RECOMP boot to
see what each override changes. Handoff brief: scratch/handoff_2026-06-18_norecomp_boot_hang.md.

## ★★ BLOCKER ROOT-CAUSED (2026-06-18, session 9): TWO independent boot-killers + reconfirmed seam
Built a group-bisection kill-switch and gdb-traced each failure mode. The NO_RECOMP boot hang is
NOT one bug — it is the hybrid-era engine colliding with a pure-Dolphin-JIT host in (at least) two
independent ways, plus the still-unsolved central seam underneath both.

### Tooling added this session
- **Override-group kill-switch** (overrides.h/overrides.cpp): every `register_override(...)` call now
  captures its `__FILE__` via a function-like macro → `register_override_impl(addr, fn, group)`.
  Under SUNBRIGHT_NO_RECOMP:
  - `SUNBRIGHT_NORECOMP_SKIP=substr,substr` — do NOT register overrides whose file matches (blacklist).
  - `SUNBRIGHT_NORECOMP_ONLY=substr,substr` — register ONLY matching files (whitelist; `__none__`
    skips ALL overrides while keeping native_os).
  - `SUNBRIGHT_NORECOMP_DBG=1` — log each skipped override.
- **native_os A/B gate** (sunbright_bridge.cpp `norecomp_skip_native_os`): `SUNBRIGHT_NORECOMP_NONOS=1`
  skips nthr at the JIT-entry seam. OPT-IN (see incoherence note below).
- **Harness**: scratch/norecomp_bisect.sh `<tag> [ENV=val...]` — 25 s headless boot, reports
  /ngxshape frame_swaps + /metrics speed + log tail.

### The matrix (all SUNBRIGHT_NO_RECOMP + FASTBOOT, headless, ~24 s)
| config | native_os | overrides | speed | state |
|---|---|---|---|---|
| NOOV (NORECOMP_NOOV=1) | off | off | 3.54× | PROGRESSES (loads nintendo.szs, zelda ucode) |
| full (default no_recomp) | on | on | 448× | RACE — VI free-runs, frame_swaps=0 |
| allov_off (ONLY=__none__) | on | off | 0.0078× | CRAWL — nthr idle deadlock |
| nonos (NONOS=1) | off* | on | 0.0073× | CRAWL — two-scheduler conflict |
| paced (PACED_BOOT=1) | on | on | 1.0× | race gone, still no draws (have_proj=1, frame_swaps=0) |
\* incoherent: recomp_lookup still routes recomp OS code into nthr — see below.

### Boot-killer #1 — VI-pacing governor race (overrides on)
gdb (full): EmuThread spins `JitTrampoline → Run → ov_VIWaitForRetrace → sunbright_wait_vi_field →
CoreTiming::Advance` forever. `wait_vi_field` advances one VI field per call bounded by
`sb_time_ahead()`; the governor only engages after the first audio push / first visual (na_ever_pushed
/ sb_visual_live). Under pure JIT the boot is stuck in a VI-wait loop BEFORE any audio/visual, so the
governor never engages → every call advances a full field → emulated time races at 448× while the
game makes no progress. PACED_BOOT=1 forces the governor on → race→1.0×, but boot still doesn't draw.
The governor's engage-on-first-audio model is recomp-boot-tuned and wrong for a JIT host.

### Boot-killer #2 — nthr idle deadlock (native_os on, overrides off)
gdb (allov_off): every guest host-thread ("CPU-GPU thread") is parked in `cond_wait` inside
`sunbright_run_recomp_tree`; the one live thread runs `nthr_idle_driver → idle_run` forever. nthr put
ALL guest threads to sleep waiting for tokens the recomp call model used to grant, and nothing wakes
them → idle driver spins (0.0078×). Dolphin's own scheduler (which boots fine under DISABLE_RECOMP)
is shadowed by nthr.

### The reconfirmed CENTRAL SEAM (why the gates are half-measures)
gdb (nonos): even with native_os gated off at the JIT entry, the spin stack is
`func_80346258 → nthrt_block_current → nthr_idle_driver` — i.e. **RECOMPILED OS code is still
running** and calling native_os. Because `recomp_lookup` (call_ppc/tail_ppc) and `recomp_raw`
(super-calls) do NOT honor no_recomp (dolphin_hook.cpp:93/108) — they always return the recomp body.
So the moment any override super-calls or `call_ppc`s, the whole recompiled subtree (incl. the GC OS
scheduler) runs as recomp, which then calls native_os. Gating native_os only at the top-level entry
therefore creates TWO schedulers over one context (the exact corruption the IsRecompiled comment
warns about) → the nonos crawl. Hence NONOS is opt-in, not default.

CONCLUSION: `SUNBRIGHT_NO_RECOMP` today only flips the TOP-LEVEL JIT entry; recomp still dominates
(~2.16M calls/s, Phase A census). "Gameplay under JIT" is not real yet. Both boot-killers AND the
gates are downstream of the unsolved seam.

### CRYSTALLIZED TARGET ARCHITECTURE (the decision)
The end state is **DISABLE_RECOMP (pure Dolphin JIT — boots cleanly today) + ENGINE overrides only.**
Overrides split into two classes; the pivot must treat them differently:
- **(A) Execution-model / pacing / OS overrides** — native_os(nthr), VI pacing (sms_vi_native /
  sms_frame_sync), GX-FIFO drawsync (sms_drawsync_lossproof / gx_stream_own / gxdrawdone), the
  host-clock governor + charge_guest_time + poll_yield device service. These exist ONLY to support
  the recomp hybrid. Under pure JIT Dolphin already does all of it correctly (proof: DISABLE_RECOMP
  boots). They must be OFF under no_recomp.
- **(B) Engine overrides** — ngx render capture (ngx_j3d_shape, scene_render, scene_id, hud), native
  audio (native_jas/se/bgm), native_card. KEEP these.

### PHASE B PLAN (concrete, the real next work)
1. **Make the seam honor no_recomp.** Under no_recomp, `recomp_lookup`/`recomp_raw` must NOT serve
   recomp bodies. With recomp off, ALL guest code runs under Dolphin JIT, which already consults
   overrides at every block boundary via the trampoline (block-linking off) — so engine overrides
   keep intercepting for free. (Interpreter stays off-limits: it bypasses overrides.)
2. **Convert class-(B) wrapping overrides from super-call to PRE-HOOK** (the journal's approach #1):
   the override OBSERVES (captures guest J3D/J2D state for ngx) then signals "run the original under
   Dolphin JIT" instead of `recomp_raw`. Mechanism: a pre-hook table consulted in
   `sb_hook_jit_trampoline` BEFORE the IsRecompiled decision; run the pre-hook, then return FALSE so
   Dolphin JITs the original block (its sub-calls re-consult overrides naturally — no reentrancy, no
   super-call). Full native-replacement overrides (e.g. ngx_j3d_shape g_native_draw) need no original
   and just return true.
3. **Turn class-(A) overrides OFF under no_recomp** (native_os via NONOS made default-on-OFF once the
   seam lands; VI/drawsync/governor via SUNBRIGHT_OVERRIDE_IF gated on "recomp-mode only").
4. Cheap validation BEFORE the big conversion: add a minimal pre-hook on ONE address atop an
   otherwise-DISABLE_RECOMP boot; confirm it boots AND the pre-hook fires (proves "pure JIT + observe
   hook, no recomp/native_os/exec-overrides" is viable) before converting all of class (B).

## ★★ PHASE B STEP 4 — TARGET ARCHITECTURE VALIDATED (2026-06-18, session 9)
Added `SUNBRIGHT_PUREJIT=1` (jit_hook.cpp): a fully-additive validation mode. In
`sb_hook_jit_trampoline` it runs an OBSERVE pre-hook on the ngx draw seams (J3DShape::draw
0x802e0390, J2DScreen::drawSelf 0x802d01c8), then ALWAYS returns false → Dolphin JITs the original
block. Ran it atop the known-good oracle: `SUNBRIGHT_DISABLE_RECOMP=1 SUNBRIGHT_PUREJIT=1
SUNBRIGHT_AUTOSTART=1 SUNBRIGHT_BACKEND=OGL` headless.

RESULT (PROVES the crystallized architecture):
- Boot PROGRESSES cleanly at **1.0× speed** — loads dolpic5.szs (Delfino), sequence.arc, THP movies.
  NO hang, NO 448× race, NO 0.0078× crawl. The pure-Dolphin-JIT boot owns OS/threading/pacing fine.
- Both engine observe-hooks FIRE: `[purejit] J3DShape::draw hit` and `J2DScreen::drawSelf hit`. So
  pure-JIT block dispatch (block-linking off) reaches the engine draw addresses → a pre-hook
  registered there WILL fire, with ZERO recomp / native_os / exec-model overrides.

=> The two boot-killers are 100% attributable to the hybrid-era exec-model overrides + native_os.
   Remove them (pure JIT) and the engine seams stay interceptable. Phase B is de-risked.
NOTE: SUSTAINED hits (heavy 3D) weren't reached in the 50 s window — the oracle can't FASTBOOT
(fastboot is an engine override, skipped by DISABLE_RECOMP) and AUTOSTART navigation into gameplay is
flaky/slow. Reaching sustained gameplay-render under pure JIT is the next validation, and depends on
getting fastboot (an engine override) working in the pure-JIT mode — i.e. Phase B step 2 proper.

### Phase B — concrete remaining work (in order)
1. **[DONE 2026-06-18]** Real pre-hook table consulted in the trampoline before the IsRecompiled
   decision: `register_prehook(addr, fn)` / `prehook_lookup` (overrides.h/.cpp). `run_prehook` in
   sb_hook_jit_trampoline builds a CPUState from live PPC state, runs the pre-hook, returns false so
   Dolphin JITs the original. Verified end-to-end: runtime/overrides/purejit_probe.cpp registers
   observe pre-hooks on J3DShape::draw + J2DScreen::drawSelf (gated on SUNBRIGHT_PUREJIT); under
   DISABLE_RECOMP+PUREJIT the `[purejit] pre-hook seams hit` line fires and boot stays at 1.0×. The
   probe + PUREJIT switch are throwaway scaffolding for the real engine pre-hooks (steps 2-3).
2. **[DONE 2026-06-18, session 10]** A real no-recomp execution mode — `SUNBRIGHT_PUREJIT` is now a
   complete SINGLE-FLAG mode (folded in the throwaway trampoline short-circuit + scaffolding):
   - `recomp_lookup`/`recomp_raw` return null under purejit (dolphin_hook.cpp) — no recomp bodies, no
     super-call bodies.
   - `native_os_lookup` returns null under purejit (native_os.cpp) — ONE chokepoint disables the nthr
     scheduler on all three consult paths (IsRecompiled / Run / call_ppc); Dolphin owns threading.
   - `IsRecompiled` returns false under purejit (sunbright_bridge.cpp) — nothing routes to Run(); the
     engine seam is PRE-HOOKS only (trampoline runs run_prehook unconditionally, cheap-rejected to
     zero when none registered, then normal dispatch).
   - Centralized `bool sunbright_purejit_mode()` (overrides.h/.cpp) — the single mode accessor.
   - **★ KEY FINDING: PUREJIT requires the DISABLE_RECOMP substrate.** PUREJIT-ALONE stalls right
     after `data/nintendo.szs` (3.76× uncapped, core_running, recomp=0 — not a race/crawl, a genuine
     early stall). Cause: the hybrid-era NON-OVERRIDE WRAP hooks (audio Mixer/AID capture, MMIO/
     gather-pipe routing, GX-FIFO drawsync, host-clock governor) stay active and are tuned for the
     recomp call model — they were never grounded for a pure-JIT boot. DISABLE_RECOMP already inerts
     ALL of them via its guard sprinkled through the runtime. So `main()` now sets
     `SUNBRIGHT_DISABLE_RECOMP` when `SUNBRIGHT_PUREJIT` is set (before any getenv-cached gate reads
     it). This refines the handoff: gating recomp_lookup/native_os/exec-model-OVERRIDES is NOT
     sufficient — there's a whole wrap-hook layer underneath, and DISABLE_RECOMP is the lever for it.
   - VERIFIED (single flag `SUNBRIGHT_PUREJIT=1 AUTOSTART BACKEND=OGL`, headless): boot PROGRESSES to
     Delfino (`data/scene/dolpic5.szs`, sequence.arc, Entrance.thp) at **1.0×**, recomp calls **0**,
     and the engine pre-hook seam fires (`[purejit] pre-hook seams hit: J3DShape::draw=1`). Default
     recomp path unaffected (smoke: 1.06×, 60M recomp calls, frame_swaps=443, ngx capturing).
   - NOTE: sustained 3D hits not yet reached — AUTOSTART cycles attract/THP; needs fastboot (step 3).
3. **[STARTED 2026-06-18, session 10 — fastboot DONE; pre-hook premise FALSIFIED]**
   - **fastboot under purejit DONE.** fastboot's two overrides (TGCLogoDir::direct,
     TMovieDirector::direct) are control-flow REPLACEMENTS (return DONE/GAMEPLAY, skip the original),
     not observe-then-run — so they CANNOT be pre-hooks. Added a `mark_override_purejit_safe(addr)` /
     `override_is_purejit_safe(addr)` mechanism (overrides.h/.cpp): a full-replacement override stays
     dispatchable via Run() under purejit. fastboot marks its two addresses safe (only when
     SUNBRIGHT_FASTBOOT is set). IsRecompiled's purejit branch now returns `override_is_purejit_safe(pc)`.
     **Found+fixed an ordering bug:** purejit sets DISABLE_RECOMP for the substrate, and IsRecompiled's
     `if (disabled) return false` was BEFORE the purejit branch → pre-empted the safe-override dispatch.
     Moved the purejit branch FIRST. VERIFIED: `SUNBRIGHT_PUREJIT=1 SUNBRIGHT_FASTBOOT=1` →
     `[fastboot] loaded save File 1 → Delfino Plaza (episode 5)` fires, boot at 1.0×, framedump
     (scratch/pjdump) shows **Delfino Plaza fully rendered via Dolphin GX** (Mario/NPCs/plaza/HUD/
     subtitle). fastboot's helper super-calls (FlagManager + OS thread join) run under Dolphin's
     interpreter via call_ppc and work (no step-budget abort) — acceptable for boot-time leaf funcs.
   - **★★★ MAJOR FINDING — the PRE-HOOK (observe + return-false) premise is FALSIFIED for per-call
     capture.** Over 60 s of rendering Delfino, the J3DShape::draw pre-hook fired exactly **ONCE**.
     Root cause: `JitTrampoline` (externals/dolphin/.../JitCommon/JitBase.cpp:101) is invoked from the
     dispatcher's CACHE-MISS/compile path (Jit64/JitAsm.cpp:214), NOT every dispatch. So:
       • **return false** (pre-hook) → Dolphin COMPILES + CACHES a passthrough block at that address;
         every later dispatch runs the cached block directly, never re-entering our hook → the
         pre-hook fires ONCE per block compile, never per call. **Useless for per-draw observation.**
       • **return true** (recomp / purejit-safe override) → we never let Dolphin cache a block there,
         so the trampoline is re-consulted on EVERY dispatch (that is why recomp + fastboot work).
     The journal's earlier "[purejit] seam hits / step-1 DONE" validation only ever saw 1 hit and
     misread it as success; the handoff's whole "convert wrapping overrides to observe-then-return-
     false pre-hooks" plan does NOT achieve per-call engine capture. CONSEQUENCE: the ngx capture
     seams must become purejit-safe **return-TRUE** seams (full native replacement that reads the
     guest-layout J3D object and draws natively, skipping Dolphin's draw — ngx already reads guest RAM,
     and ngx_j3d_shape::g_native_draw is exactly such a replacement), OR use a reentrant run-under-JIT
     primitive (journal approach #2) for the few seams that must run the original AND observe. Pre-
     hooks remain valid only for ONE-SHOT interception (patch-once-at-first-compile), not capture.
4. **[STARTED 2026-06-18, session 10 — DRAW seam DONE]** Convert the ngx capture seams to purejit-safe
   return-TRUE overrides.
   - **J3DShape::draw (0x802e0390) DONE + VERIFIED.** g_native_draw forced on under purejit (no recomp
     body to super-call; ngx draws natively); the override marked purejit-safe when ngx capture active.
     RESULT: under `SUNBRIGHT_PUREJIT=1 SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_NGX_SHAPE=1 BACKEND=OGL`, the
     draw seam fires PER-CALL — /ngxshape calls=569684, meshes_built=569684, verts=178M, tris=106M
     over ~45 s. This PROVES the return-true full-replacement seam captures every shape under pure JIT
     (vs the once-per-compile pre-hook). The biggest recomp consumer is now a native draw under JIT.
   - REMAINING capture seams (same return-true pattern, decide native-replace vs run-original per seam):
     the GX-tee captures in ngx_j3d_shape.cpp — ov_gxsetcopyclear (0x8035ea40, clear color ngx needs),
     ov_gxsetfog, ov_gxload{tlut,texobj,texobjpreloaded,lightobjimm}, ov_gxsetchan{ctrl,matcolor,
     ambcolor}, ov_gxloadposmtx{imm,indx}, ov_j3dtexmtx_load, ov_j3dgd* — plus scene_render
     (GXSetProjection, J2DScreen draw) and hud. Each currently OBSERVES then super-calls the original
     GX function; under purejit they must dispatch per-call (return-true) and either native-replace the
     GX side effect (ngx reads guest objects, so most GX writes are discardable when ngx presents) or,
     where the original's side effect is genuinely needed, use a reentrant run-original primitive.
   - **ALL GX-tee capture seams DONE (commit e7521f4).** ngx_super(cpu,addr) helper: no-op under
     purejit (capture-only; ngx owns rendering, GX side effect discarded), super-call under recomp.
     All 16 tees routed through it + marked purejit-safe. Verified full breadth under purejit: 91
     material states, 75k light loads / 65M lit verts, per-texmap textures (54 distinct), PE FL=566k,
     copy-clear 4829 sets.
   - **★★★ DELFINO RENDERS NATIVELY UNDER PURE JIT (commit 3a58518).** GXSetProjection + the J2DScreen
     frame-publish boundary converted to purejit-safe per-call (scene_render.cpp). RESULT:
     `SUNBRIGHT_PUREJIT=1 FASTBOOT=1 NGX_PRESENT=1 BACKEND=Vulkan` renders Delfino Plaza via the
     PC-native ngx renderer with ZERO recomp — ngx_present_live init_ok=1 frames=1014 pipelines=92
     textures=180; framedump (scratch/pjpresent) shows Mario/NPCs/palms/plaza/buildings. **The
     Vulkan-under-DISABLE_RECOMP "dies at 3D entry" gotcha did NOT reproduce under purejit** — Vulkan
     present works. This is the full PC-native engine rendering SMS gameplay under pure Dolphin JIT.
   - **REMAINING (the reentrant primitive):** the HUD/2D overlay is missing under purejit because
     J2DScreen::draw (and TGCConsole2::perform) are LOGIC functions that BUILD the J2D pane tree
     (mGlobalBounds) ngx's overlay reads — they cannot just skip the original. Running the original
     under pure JIT needs a **reentrant run-original-under-Dolphin-JIT primitive** (journal approach
     #2): inside Run(), JIT+execute the original block at em_address and return, so the override can
     observe AROUND it. This is the one genuinely-missing mechanism; once it exists, J2DScreen/perform
     become observe-around-original (HUD overlay returns) and any other "needs the original's side
     effect" seam is unblocked. Then A/B pixels (tools/render/ab_diff.py) vs the recomp path, then
     Phase C (unlink generated/, drop the recompiler from the build, JIT-native default).

## ★★★ PHASE B STEP 5 — THE REENTRANT RUN-ORIGINAL PRIMITIVE — DONE + VERIFIED (2026-06-18, session 11)
The one genuinely-missing mechanism is BUILT. **The HUD/2D overlay now renders under pure Dolphin
JIT with ZERO recomp** (coin counter + FLUDD gauge visible in the ngx present; hud_quads 0→6).

### Mechanism — `sb_run_original_around(cpu, addr, after, cookie)` (overrides.h / dolphin_hook.cpp)
A purejit-safe WRAPPING override (return-true, dispatched per call via Run()) cannot run the ORIGINAL
inline (no recomp body to super-call; call_ppc → interp would BYPASS overrides). The primitive runs
the original under Dolphin's OWN JIT by THREADING BACK THROUGH the dispatcher — NOT by nesting a
second enter_code or longjmp-ing across one (both rejected as risky: ProtectStack/UnprotectStack +
the BLR-RAS sentinel). We are inside Run() ← JitTrampoline ← dispatcher; the override calls the
primitive and returns, then:
  1. **One-shot bypass** (g_bypass_once_addr, thread-local): IsRecompiled(addr) returns FALSE on the
     NEXT dispatch only → Dolphin JITs+runs the ORIGINAL block instead of re-dispatching the override
     (the infinite loop). Keyed to addr → consumed exactly when the dispatcher reaches it.
  2. **Explicit-next-pc** (thread-local, consumed in sunbright_run_recomp_tree's epilogue): the
     override's continuation pc is `addr` (the original) while its guest LR is set to a TRAP sentinel
     (0x80002FF0, the low-mem gap). So the original runs with entry-LR=TRAP; the dispatcher resumes at
     addr, not at cpu.lr. (Unarmed = the universal case = a single TLS-bool test → pc=cpu.lr as before.)
  3. The original runs at full JIT speed; its sub-calls hit the trampoline (block-linking is off →
     every block returns to the dispatcher) so the GX-tee / draw overrides keep intercepting. Its
     final blr mispredicts (no RAS entry — entered via dispatcher JMP) back to the dispatcher with
     pc = restored entry-LR = TRAP.
  4. **TRAP** is a purejit-safe override (sb_around_trap): it **InvalidateICache(addr,4,forced)** —
     drops the original's freshly-cached entry block so the WRAPPING override is consulted again next
     call (cache-miss → trampoline → IsRecompiled==true) instead of Dolphin running the now-cached
     passthrough directly — then runs `after(cookie)` (the "after" work, e.g. sb_j2d_capture), then
     resumes the real caller (cpu.lr = the saved real return). The block invalidation is THE key
     subtlety: without it the bypass would permanently defeat interception (Dolphin caches the block).
GATING: the TRAP override is registered ONLY under purejit (its sentinel addr is far below every real
override; registering it in recomp mode would widen override_lookup's [lo,hi) cheap-reject window and
force a hashmap probe on the hot call_ppc/blr path). Inert under recomp — the primitive is purejit-only.

### Wiring + verification
- `ov_j2dscreen_draw` (scene_render.cpp) under purejit: before-work (ngx_frame_publish + set_root) as
  before, then `sb_run_original_around(cpu, J2DSCREEN_DRAW, &sb_j2d_capture, root)` instead of the
  old `return` (which skipped the original → empty pane tree).
- VERIFIED (`SUNBRIGHT_PUREJIT=1 FASTBOOT=1 NGX_PRESENT=1 BACKEND=Vulkan`, headless): `/j2d` shows a
  POPULATED pane tree (draws=4836, PIC1 panes with real mGlobalBounds — proof the original ran);
  `/abshot2` → init_ok=1, **hud_quads=6**; the ngx frame (scratch/screenshots/ab2.ngx.png) shows
  Delfino Plaza + the HUD coin counter and green FLUDD gauge composited over it. recomp calls = 0.
  Stable to 1.4M+ J3D draws / 4836 J2D draws, no crash, speed ~0.56× headless.
- DEFAULT recomp path still GREEN (gating inert): speed 1.20×, recomp 11.5M calls, hud_quads=14.
- GOTCHA learned: headless WITHOUT SUNBRIGHT_DUMP never drives the Presenter, so /ngxpresentlive
  reads init_ok=0/frames=0 — use `/abshot2` (on-demand ngx render) to force a present + read stats.
  And the GX XFB is NOT a valid oracle under purejit (ngx replaced the 3D draws → Dolphin GX is
  missing them; ab_diff vs gx = meaningless). A/B fidelity must be purejit-ngx vs recomp-path.
- LIMITATION (documented in code): the primitive needs the original to RETURN (blr to TRAP). A
  function that tail-branches away and never returns would leak the around-frame + never run `after`.
  Fine for J2DScreen::draw / perform (normal returning functions).

NEXT: Phase C — stop linking generated/, drop tools/recompiler from the BUILD + the recomp call
model, make JIT-native the default. (TGCConsole2::perform can also adopt the primitive if it needs
observe-around, but it already runs naturally under JIT when not marked purejit-safe.)

## ★★ PHASE C READINESS — VALIDATED (2026-06-18, session 11)
Before any destructive build change, confirmed the JIT-native mode is a COMPLETE substrate, not just
fastboot-into-Delfino:
- **Normal boot under pure JIT (no fastboot) reaches gameplay.** `SUNBRIGHT_PUREJIT=1 AUTOSTART=1
  NGX_PRESENT=1 BACKEND=Vulkan` (headless): boots logo → (AUTOSTART) title → `data/scene/option.szs`
  (file-select) → `data/scene/dolpic5.szs` (Delfino) + the entrance THP — all under pure Dolphin JIT,
  **recomp calls = 0**, stable at **1.0×** for 100s+, no crash/fatal (only a benign IPL font warning).
  frame_swaps climb ~30/s (1 per VI field). So menus + file-select + scene-load + gameplay all work
  under pure JIT, refuting the journal's earlier "AUTOSTART flaky, needs fastboot" worry.
- **Render fidelity is the SAME as recomp** (both use ngx on the same guest J3D objects; gameplay
  logic runs on Dolphin JIT — which is MORE correct than our recomp's known FP/edge bugs). The ~45%
  black in the AUTOSTART shot was the THP entrance MOVIE region (ngx doesn't composite THP video) +
  transition — a pre-existing ngx gap identical in recomp mode, NOT a purejit regression. fastboot →
  Delfino renders nearly full under purejit (the plaza/Mario/NPCs/HUD shot).
- **CONCLUSION: JIT-native (purejit) is render-equivalent and execution-superior to recomp.** Phase C
  is sound; making it the default does not regress fidelity.

### Phase C scope (read before starting — it's COUPLED, not three independent edits)
"Stop linking generated/" + "make JIT-native default" + "drop recomp call model" are interdependent:
once generated/ is unlinked, JIT-native is the ONLY working mode, so EVERY wrapping override that
super-calls `recomp_raw(addr) ? o(cpu) : call_ppc(cpu, cpu.lr)` must FIRST work under JIT-native.
- Build linkage (CMakeLists.txt:123): `SUNBRIGHT_GENERATED_SOURCES` globs generated/functions_*.cpp
  + jump_table.cpp into the sunbright binary; a weak `g_recomp_table` fallback lets it link empty.
  Unlinking = drop that glob from add_executable; g_recomp_table empties → recomp_lookup always
  misses. The recomp CALL MODEL (call_ppc/recomp_raw/dolphin_hook.cpp) stays linked but serves null.
- The ~10 recomp_raw super-call overrides (memory no-recomp-jit-native-direction): render path is
  DONE (scene_render = return-true + the new reentrant primitive; ngx_j3d_shape = g_native_draw).
  REMAINING to assess one-by-one under purejit: hud.cpp (ov_quad/ov_water_posmtx), water_native,
  sunmodel_widescreen / screenfx_widescreen / efbtex, scene_id, native_gx, fastboot helpers. Each is
  currently NOT purejit-safe → inert under purejit (runs naturally under JIT). Decide per override:
  is "natural JIT, no wrapper" correct (the wrapper only did a widescreen tweak ngx now owns), or does
  it need a return-true native replacement / the reentrant primitive? Validate each headless.
- SAFE ORDER: (1) per-override: confirm each recomp_raw site is correct-or-converted under purejit;
  (2) flip the default to JIT-native behind an opt-OUT flag (keep recomp for A/B) + re-validate the
  whole game; (3) only AFTER soak, the irreversible cut: drop SUNBRIGHT_GENERATED_SOURCES + the
  sunbright-recomp build target (keep it as offline static-analysis tooling per the user directive).
  Don't do (3) before (1)+(2) — that removes the A/B fallback the journal's whole method relies on.

## ★★ PHASE C — STEP 1 ASSESSED (2026-06-18, session 12): NO conversions needed
Assessed every `recomp_raw` super-call WRAPPING override under purejit (headless fastboot Delfino +
abshot2 A/B vs recomp-path ngx). **All of them correctly fall to "natural JIT, no wrapper" (decision
(a)) — none changes the ngx-presented output, so none needs converting to return-true / the reentrant
primitive.** They stay LIVE in recomp mode (the A/B fallback through Step 2), inert+harmless under
purejit (registered but never dispatched — IsRecompiled returns override_is_purejit_safe()==false).

WHY they're all dead-or-irrelevant under purejit (the unifying principle):
- **ngx reads the OBJECT MODEL, not Dolphin GX.** The J2D HUD overlay reads pane `mGlobalBounds`@0x24
  (raw 0..640 screen space, j2d_walk.cpp), NOT the draw-time position matrices these overrides poke.
  PROVEN: purejit (anchoring inert) vs recomp-path (anchoring active) Delfino HUD shots are pixel-
  identical at the corners → hud.cpp ov_quad/ov_water_posmtx have ZERO effect on ngx output.
- **The widescreen tweaks only matter for the Dolphin-GX raster path.** ov_gx_projection IS purejit-
  safe and publishes the squeezed PERSPECTIVE to ngx (3D widescreen FOV ✓). The 2D-squeeze / HUD
  edge-anchor / water-lookup-m00 / sun-occlusion-reproject all only fix where geometry lands in
  Dolphin's EFB — which ngx discards. At base aspect (4:3) water_native is a literal no-op
  (`if (s==1.0f) return`); sunmodel/screenfx/efbtex are EFB-roundtrip effects.
- **EFB-readback effects are a PRE-EXISTING ngx-owns-present gap, NOT a Phase C regression.** Under
  purejit `ngx_super` skips the original GX draw (ngx_j3d_shape.cpp:75) → Dolphin's EFB is not
  populated for 3D → guest `GXPeekZ` (sun occlusion glow/lens flare), `GXReadPixMetric` (pollution
  goop-coverage % → episode triggers), the mirror pre-render, and TAfterEffect dash-blur all read an
  empty/stale EFB. This is identical under recomp+NO_RECOMP+NGX_PRESENT (g_native_draw on there too)
  and is the broader "cut Dolphin's GPU tie" frontier — flagged, out of Phase C scope. NOTE: the
  current DEFAULT (recomp + NGX_PRESENT, no no_recomp) does NOT skip the GX draw (g_native_draw off)
  → Dolphin EFB stays real → these effects work today. So flipping the default to purejit DOES regress
  EFB-readback effects vs the headed default — surfaced to the user before the flip.
- **Diagnostics / opt-in / feature-gated** are inert by design: scene_id (SUNBRIGHT_2DID off),
  native_gx (SUNBRIGHT_NATIVE_GX off), hud_probe/beam_diag/jas_rate_diag/seqparser_diag/dbg_thp_psq;
  native_si/native_exi/native_mi/native_math/se_native/ttrack/sms_jkrthread run the guest body when
  their feature is off → under purejit Dolphin JIT runs the guest function faithfully.
- fastboot helpers + render seams already purejit-safe (GCLOGO/MOVIE_DIRECT, GX_SET_PROJECTION,
  J2DSCREEN_DRAW, the ngx GX-tee list). Nothing to do.
OPEN for Step 2 validation (not a wrapping-override, so not Step-1): AUDIO under purejit. The native
audio engine intake (se_native/njas) is NOT purejit-safe → inert → audio falls to the guest JAS path
running under Dolphin JIT + Dolphin DSP HLE (and the native_audio Mixer WRAP sink is inerted by the
DISABLE_RECOMP substrate purejit implies). Must verify headless with SUNBRIGHT_DUMP_AUDIO in Step 2.

## ★★★ PHASE C DONE — RECOMP ERADICATED FROM THE GAME (2026-06-18, session 12)
User reordered the plan: "recomp should be ERADICATED FIRST then the flip" + "too many flags" (so NO
SUNBRIGHT_USE_RECOMP opt-out) + "previous flip attempts failed but those were recomp-related so be
careful still". Executed as three validated, pushed commits (recomp=0 throughout; generated/ now
reports "Linked 0 recompiled functions"):

- **C-A (01ecee5) — runtime eradication.** `sunbright_purejit_mode()` is unconditionally true (one
  execution mode: gameplay on Dolphin JIT, engine as native overrides). main_sdl sets the
  DISABLE_RECOMP substrate unconditionally. **Decoupled render-ownership from purejit** via new
  `ngx_capture_active()` (env-computed, static-init-order-safe — NOT g_enabled, which other TUs'
  static registrars race): scene_render's GXSetProjection publish-and-skip + its purejit-safe mark,
  and the J2DScreen reentrant-around + its mark, now gate on ngx_capture_active(). So **the Dolphin-GX
  baseline survives recomp eradication**: run with ngx capture OFF (no NGX_PRESENT/NGX_SHAPE) →
  guest GX/J2D draws run under Dolphin's JIT → Dolphin's GX output is on-screen (the ngx oracle).
- **C-B (0fa949c) — the build cut (irreversible).** Dropped `${SUNBRIGHT_GENERATED_SOURCES}` from the
  `sunbright` add_executable; recomp_table_weak.cpp's weak-empty g_recomp_table is the only table.
  Binary 104MB→76MB. `sunbright-recomp` tool KEPT as offline static-analysis tooling. LINK FIX =
  `runtime/recomp_dead_stubs.cpp`: ~23 recomp-era override/diagnostic files still NAME generated
  bodies directly (super-call-via-direct-`func_XXXXXXXX`-symbol). All such calls are in paths INERT
  under no-recomp (the override isn't purejit-safe → never dispatched → Dolphin JIT runs the
  original). 72 weak abort-stubs satisfy the linker for that dead code; verified-unreachable, so a
  stub aborts loudly naming itself if the deadness analysis is ever wrong. TRANSITIONAL — the stubs
  go as the dead super-call/diagnostic code is deleted.
- **(17fa6ac) — collapse IsRecompiled** to the single no-recomp path (the whole recomp-dispatch tail
  was dead). A function is "recompiled" iff it's a full-replacement override marked purejit-safe,
  except the reentrant primitive's one-shot bypass.

VALIDATION (headless, eradicated build): fastboot Delfino renders identically to the prior purejit
baseline (plaza/Mario/NPCs/HUD, hud_quads=15, init_ok=1); NORMAL boot (no fastboot, AUTOSTART) goes
logo→file-select(option.szs)→Delfino(dolpic5.szs)→gameplay at ~1.0×; ngx-off baseline boots to
Delfino under pure Dolphin JIT — all recomp=0, no fatal, no dead-stub abort. `./run.sh` (NGX_PRESENT
default) is now the JIT-native experience with no flags.

REMAINING (follow-ups, NOT blockers — system is correct as-is):
- Dead-code deletion: delete the inert recomp-era super-call/diagnostic override files + their dead
  stubs (the C-B link fix is transitional scaffolding). Careful/incremental.
- Vestigial mode-flag reads (SUNBRIGHT_PUREJIT / NO_RECOMP / NGX_NATIVE_DRAW / NORECOMP_*): now
  no-ops or redundant; prune for the "too many flags" goal.
- ★ KNOWN no-recomp GAPS (surfaced to user, accepted to address later — see Step-1 section above):
  (1) the native_jas AUDIO ENGINE is DORMANT under no-recomp (its tees aren't purejit-safe → audio
  routes guest-JAS-under-Dolphin-JIT → Dolphin DSP HLE → native sink; underruns observed). To honor
  the "full PC engine" goal it must be made purejit-safe. (2) EFB-readback effects (sun-glow/lens-
  flare occlusion, pollution goop-coverage triggers, mirror, dash-blur) read an empty EFB because ngx
  skips the GX draw — the broader "own the GPU" frontier.

## ★★★ POST-ERADICATION: THE ENGINE IS RENDERER-ONLY — re-grounding plan (2026-06-18, session 12)
USER (ground truth, ran ./run.sh): "the game is NOT running under a PC native engine." CONFIRMED.
Eradicating recomp didn't just remove recomp — it left the native engine as the **renderer only**.
Diagnosis (verified): under no-recomp an override runs ONLY if marked purejit-safe, and the ONLY
purejit-safe overrides are fastboot + the ngx render seams (GX_SET_PROJECTION, J2DSCREEN_DRAW, the
ngx GX/J2D tee list). Every other native subsystem is INERT → Dolphin does that work:
- native_jas AUDIO ENGINE — dormant (audio = Dolphin DSP HLE into the native sink, which IS active
  via the Mixer WRAP). se_native/bgm intake tees are plain overrides → inert.
- native_card — registers via native_os_register → native_os_lookup returns null under purejit → inert
  → Dolphin's own EXI/card serves it (works; fastboot loads the save).
- sms_drawsync, native_os/nthr, native_gx, water/efb/sun widescreen, native_math — all inert.
USER DIRECTIVE on scope: **"Game engine entirely PC native."** So re-ground ALL owned engine
subsystems to run under no-recomp (NOT the recomp-bugfix overrides — those are correctly dead because
Dolphin's JIT executes the guest correctly where our recomp mistranslated; e.g. native_math/fres
refiners are unneeded under Dolphin JIT).

### THE LINCHPIN BLOCKER (why this is multi-session, not a flag flip)
Native engine subsystems CALL GUEST HELPER FUNCTIONS and need the result INLINE:
- native_card → func_8035796c (__CARDVerify, reads c.gpr[3] back), func_8035532c, callbacks via
  call_ppc; se_native → func_803020ac (guest startSoundBasic) for ids njas doesn't handle; etc.
- Those `func_XXXXXXXX` recomp bodies are GONE (now recomp_dead_stubs.cpp → abort). `call_ppc(cpu,
  addr)` under no-recomp would run the guest under the INTERPRETER, which BYPASSES overrides
  (forbidden) and is slow.
- The existing reentrant primitive `sb_run_original_around` does NOT solve this: it runs the original
  of the OVERRIDDEN function via dispatcher-threading (return-from-override → Dolphin JITs it → trap
  back), NOT a synchronous value-returning nested call from inside native C.
TWO STRATEGIC PATHS to "entire engine native" (pick per subsystem; A unblocks broadly):
- **(A) Build a synchronous nested guest-call primitive** `sb_call_guest(cpu, addr)` that runs a guest
  function at addr under Dolphin's OWN JIT and returns with results in cpu — sub-calls still hitting
  overrides. Then native subsystems keep their guest-helper-calls (re-point func_/call_ppc at it) AND
  IsRecompiled can dispatch all engine overrides. This is the nested-enter_code the prior author
  avoided as "risky (ProtectStack/UnprotectStack + BLR-RAS sentinel)" — but it is the general enabler
  and the reentrant primitive proves dispatcher-threaded nested exec works; a value-returning blocking
  variant is the build. HIGH LEVERAGE.
- **(B) Fully port each subsystem + ITS helpers natively** (RE __CARDVerify etc.), no guest calls —
  the project's faithful methodology, but enormous per subsystem. native_jas already is ~full port
  (synthesizes everything); audio mostly needs (A) only for the unhandled-id fallback + double-audio
  suppression (guest audio now PLAYS under Dolphin, so njas must suppress the Dolphin path to avoid
  doubling).
RECOMMENDATION: build (A) — it is the one mechanism that re-grounds the whole engine; then per
subsystem, mark seams purejit-safe + re-point guest-helper-calls at sb_call_guest, validating no boot
stall (the old "stall a pure-JIT boot" risk lives in the TIMING subsystems: governor/AID/Mixer/
drawsync — Dolphin owns timing under no-recomp, so those stay Dolphin unless/until ported with care).
Tasks: #5 audio, #6 card, #7 drawsync, #8 inventory+remaining. Per-subsystem validate headless +
commit; never leave a subsystem half-re-grounded (partial audio is worse than Dolphin-HLE audio).

## ★★ RE-GROUNDING PATTERN ESTABLISHED + AUDIO DONE (2026-06-18, session 12, commit bc67e00)
The re-grounding recipe (proven on audio; applies to every engine subsystem):
- **`SUNBRIGHT_OVERRIDE_NATIVE(name, addr)`** (overrides.h) = register + mark purejit-safe in one →
  the seam DISPATCHES under no-recomp (runs native, returns to lr). Use for FULL-REPLACEMENT seams.
- **Fallback / guest-original needed** (a sound id the engine doesn't own, a stream, an A/B-disable):
  `run_guest(cpu, addr)` = `sb_run_original_around(cpu, addr, nullptr, 0)` — runs the guest original
  under Dolphin's JIT so nested overridden calls still intercept (interp would bypass them). Must be
  the override's LAST action.
- **Leaf guest helper whose RESULT is needed inline** (e.g. __CARDVerify → r3): `call_ppc(cpu, addr)`
  runs it under the interpreter and returns inline. OK only when the helper's subtree has no
  override-relevant calls (else use run_guest, which keeps interception but isn't value-returning).
- **Pure observe-then-super-call wrappers** that just funnel into another overridden fn: LEAVE INERT
  — the guest runs them under JIT and the nested call hits the override (e.g. startSoundActor/
  startSoundDirectID → startSoundBasic).
- **No double-output rule**: a full-replacement intake (skips the guest body) means the guest engine
  never gets those voices → its Dolphin output is silent for them → the native engine is the sole
  source automatically (no suppression needed). PROVEN for audio: sink RMS ≈ njas-only (ratio 0.98).

AUDIO (native_jas) RE-GROUNDED + verified: se_native tees → OVERRIDE_NATIVE; njas synthesizes SE/BGM
natively under no-recomp (recomp=0, 1.0×, no boot stall), real smooth audio, no doubling. The
guest-JAS-path overrides (cmdnoteon/oscillator/dsp_update/ttrack/aid/syncdsp/zelda_ucode) stay INERT
— they're for the guest audio path njas bypasses; Dolphin owns DSP/mailbox timing. Engine now native
under no-recomp = **renderer (ngx) + audio (native_jas)**. NEXT: card (#6 — __CARDVerify via call_ppc,
switch native_os_register→OVERRIDE_NATIVE), drawsync (#7), inventory the rest (#8).

## ★★ PROOF: rendering + XFB ARE PC-native under no-recomp (2026-06-18, session 12)
User doubted ./run.sh is full PC-native rendering/XFB. MEASURED via /abshot2 (Present.cpp captures
Dolphin's GX XFB and the ngx frame from the SAME present):
- **Dolphin GX XFB = 100% BLACK** (avg (0,0,0), 0.000 non-black) — the guest 3D draws are skipped
  (ngx_super returns under purejit), so Dolphin's GX renders nothing.
- **ngx frame = the full scene** (avg (130,134,125), 0.894 non-black).
Airtight logic: you SEE the game in ./run.sh, but Dolphin's GX XFB is black → the on-screen image is
necessarily the ngx native frame (the fork Presenter substitutes ngx's texture for the XFB via
sb_ngx_present_xfb; else "keeps the real XFB" = black). Rendering AND XFB are native.

interp60 FINDING: the live interp60 (interp_redraw.cpp) is GX-COMMAND-STREAM REPLAY — gxs_cur_frame()
+ gxs_replay_frame() re-render the captured GX FIFO twice through Dolphin's OpcodeDecoder into
Dolphin's EFB, present Dolphin XFB phys addrs (sb_present_xfb). Under no-recomp+ngx the GX stream is
empty (draws skipped) → interp60's replay has nothing → it CANNOT run on ngx as-is (same fact as the
black GX XFB). "interp60 on the native engine" = REIMPLEMENT on ngx's object model: in-between field
blends per-model draw matrices (the object-model ov_j3d_viewCalc_blend mode-3 path, not GX replay),
re-issue the perform-list draw so ngx captures the blended matrices, present NGX's frame twice (real +
blended) instead of sb_present_xfb of a guest XFB. Task #9. Gated by SUNBRIGHT_INTERP60 (safe to
default). This is a real focused build, not a flag flip.

## Files
- runtime/jit_hook.cpp — `sb_hook_jit_trampoline` runs `run_prehook` then normal dispatch (step 2).
- runtime/overrides.h / overrides.cpp — `sunbright_purejit_mode()` accessor + pre-hook table;
  register_override → register_override_impl(group); NORECOMP_SKIP/ONLY.
- runtime/native_os.cpp — `native_os_lookup` returns null under purejit (the native_os chokepoint).
- runtime/dolphin_hook.cpp — `recomp_lookup`/`recomp_raw` return null under purejit; SUNBRIGHT_DBG_CPBT.
- runtime/main_sdl.cpp — `SUNBRIGHT_PUREJIT` implies `SUNBRIGHT_DISABLE_RECOMP` substrate (top of main).
- runtime/overrides/purejit_probe.cpp — observe pre-hooks on J3DShape/J2DScreen (step-3 placeholder).
- runtime/sunbright_bridge.cpp — `IsRecompiled` purejit branch (step 2) + no_recomp branch (Phase A).
- scratch/norecomp_bisect.sh — boot-hang bisection harness.
- runtime/overrides/ngx_j3d_shape.cpp — `g_native_draw` + ov_j3dshape_draw native per-view setup.
- runtime/overrides.h — SUNBRIGHT_OVERRIDE_IF (conditional registration).
- runtime/overrides/{dbg_logo,interp_redraw,interp_capture}.cpp — feature-gated registration.
- runtime/dolphin_hook.cpp — SUNBRIGHT_DBG_CPBT (call_ppc one-shot host backtrace); ★ the reentrant
  primitive: `sb_run_original_around` + `sb_around_trap` + `sb_bypass_once_check` +
  `sb_consume_explicit_next_pc` (explicit-next-pc consumed in sunbright_run_recomp_tree's epilogue).
- runtime/sunbright_bridge.cpp — IsRecompiled purejit branch consults sb_bypass_once_check first.
- runtime/overrides/scene_render.cpp — ov_j2dscreen_draw uses the primitive under purejit.
- Memory: no-recomp-jit-native-direction.

## ★★ RE-GROUNDING INVENTORY COMPLETE (task #8) + native_card DONE (2026-06-18, later session)
native_card RE-GROUNDED (commit pushed): the 5 SDK seams (Probe/Mount/ReadSegment/WritePage/Erase)
now register as purejit-safe full-replacement overrides via a STATIC INITIALIZER (the native_os_init
path is dead under the eradicated build — recomp_build_dispatch is never called); the 4 guest leaf
helpers whose recomp bodies were eradicated (__CARDVerify/__CARDPutControlBlock/__OS(Un)LockSramEx)
run via call_ppc. VERIFIED: AUTOSTART → CARDMountAsync dispatches → mount verify=0 → file-select loads.

INVENTORY of every still-inert override under no-recomp (the `--xref`/`--callees` + grep census):
there is NO genuine game-engine (class-B) subsystem left to re-ground. Engine now PC-native for the
three subsystems that matter: **renderer (ngx), audio (native_jas), memory-card (native_card)**.
Everything else inert under no-recomp falls into:
- **Recomp-correctness fixes** (matrix_native, native_math fres/frsqrte, etc.): correctly DEAD —
  Dolphin's JIT executes the guest math correctly, so our recomp-era refiners are unneeded.
- **Class-A OS/HW/pacing** (native_os/nthr threading, native_si/exi/mi, sms_os_intr/memprotect,
  sms_lc, sms_drawsync_lossproof, VI/governor): Dolphin owns these correctly under pure JIT. Moving
  them native is the journal's risky path and needs sb_call_guest; NO functional gain (drawsync
  specifically would re-introduce the hybrid token-ordering machinery → risks the Delfino freeze).
- **EFB/widescreen** (water/efb/screenfx/sunmodel/cull/fillrect/fader _widescreen, efbtex): ngx owns
  the projection; the rest only positioned geometry in Dolphin's EFB, which ngx discards.
- **Diagnostics / opt-in / guest-JAS-path** (native_gx, dbg_*, oscillator/dsp_update/cmdnoteon for
  the guest audio path njas bypasses): inert by design.

CONCLUSION: the "re-ground owned subsystems" phase is COMPLETE. The two remaining native-engine
frontiers are both major multi-session builds: (1) **sb_call_guest** (value-returning nested
guest-call under Dolphin JIT) to re-ground class-A threading/timing native — speculative, no current
consumer, risky; (2) **own-the-GPU / EFB** so ngx populates a readable EFB → the missing EFB-readback
effects work (sun-glow/lens-flare occlusion, pollution darkening [the parked "wash"], mirror,
dash-blur) AND THP movie video composites into the ngx present (the ~45% black entrance). Frontier (2)
is the real remaining "engine" gap with VISIBLE missing features.
