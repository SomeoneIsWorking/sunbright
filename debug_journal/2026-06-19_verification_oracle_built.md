# 2026-06-19 — Built the working ngx-vs-GX pixel oracle (tooling-first hard rule)

## Task
Per the tooling-first hard rule (CLAUDE.md "🔧 TOOLING / VERIFICATION FIRST") and the handoff
`scratch/handoff_2026-06-19_verification_oracle.md`: the renderer-fidelity verification harness was
reported broken (both documented oracles produce garbage). Fix the harness BEFORE any fidelity work.

## What I found
1. **`oracle_ab.sh` already existed and already works.** It was committed earlier (d20acbd) but the
   handoff didn't account for it. It runs TWO fastboot processes (NGX_PRESENT=0 = Dolphin-GX oracle,
   =1 = ngx), syncs them on `emu_secs`, and captures each via `/abshot2` (which DOES write a valid
   `ab2.gx.ppm` in the `=0` process — the black-oracle problem only affects the single-present case).
   VERIFIED on real plaza data: **18.1% / 18.0%** across two runs; per-side cross-run drift **0.5%
   (GX) / 0.2% (ngx)** — i.e. deterministic, the signal swamps the drift. So the premise "both oracles
   are broken" was outdated for the fastboot path.

2. The single-present `/abshot2` GX oracle is genuinely dead under no-recomp (ngx owns the frame →
   Dolphin XFB black). `ab_diff.py`'s empty-guard (exit 3) correctly refuses it.

3. `SUNBRIGHT_STATE` auto-load is genuinely broken on the native path (field trigger dies ~field 1450;
   main-thread `State::LoadAs` deadlocks the governor-parked CPU). Confirmed.

## What I built
- **`/loadstate?f=<path>` probe endpoint** (`runtime/probe_server.cpp`): loads a save state on the CPU
  thread via `Core::RunOnCPUThread` (the probe runs on its own thread → PauseAndLock + queue + run on
  resume; no main-thread deadlock). This is the sanctioned cross-thread state-load path.
- **`tools/render/ab_oracle.sh <save.sav> [settle_s]`**: the SAVE-STATE two-process oracle. Each
  process fastboots to a running core, `/loadstate`s the SAME save (identical restored RAM ⇒ frame-exact),
  then `/abshot2` captures its renderer's PPM. Reaches ANY saved scene (not just fastboot plaza), so
  sun-occlusion / sphere-sky scenes become verifiable once driven-to and saved. Auto-picks the newest
  `build*/sunbright` (a stale `build/` lacking `/loadstate` would fail with "unknown path").

## Key finding — save states are renderer-state-blind (stale saves crash)
A Dolphin save restores only RAM + PPCState, NOT the native engine bookkeeping (ngx / native_jas /
threading). Loading a **stale** save (the Jun-3 `scratch/{delfino,gameplay,hud_gameplay,quick}.sav`,
made pre-no-recomp-pivot) restores a guest SP from a different execution model → corrupted r1
(`0xff7a75d0`) → FATAL invalid guest read → abort. A **fresh** save made under the current build
round-trips cleanly (tested: load → no crash, emu continues). Made `scratch/fresh_plaza.sav`
(`SUNBRIGHT_SAVE_STATE=… SUNBRIGHT_SAVE_AT=70`, NGX_PRESENT=1 because `SAVE_ON_HUD`'s counter only
ticks under ngx present).

## Verification of the tool itself (tooling-first, recursively)
`ab_oracle.sh scratch/fresh_plaza.sav 5`: **18.4% / 18.5%** across two runs; GX cross-run drift 0.6%,
ngx 0.0%. Non-black both sides (no exit-3). Cross-validates `oracle_ab.sh` (~18% on the same scene via
a totally independent sync method). render_test still 1/1 (10/10 internal).

## State of the oracles now
- `oracle_ab.sh` — fastboot+emu_secs, plaza only, no save needed.
- `ab_oracle.sh` — save-state, ANY saved scene, deterministic. **Preferred.**
- Both feed `ab_diff.py` (empty-guarded). Plaza ngx-vs-GX = ~18% (the historical "40%" was the
  empty-oracle artifact). Fidelity work is now UNBLOCKED.

## Next (was gated on this, now open)
Drive+save fresh states at the sun-occlusion / sphere-sky scenes → `ab_oracle.sh` them → port those
EFB-readback effects with a real number to move. The Delfino floor "wash" is still PARKED per user.

---

# 2026-06-19 (cont.) — GXDrawSphere capture: geometry VERIFIED, end-to-end UNVERIFIABLE (hard-rule STOP)

Continuing ("keep going"), I used the new oracle to pick a non-parked engine gap. The documented
candidate was "ngx misses immediate-mode GXDrawSphere" (the `Map/Sky.cpp:88` skybox dome). I ported it
the same way as the existing GXDrawCube capture:
- `ngx_imm_geom.h`: `sphere_verts` / `sphere_tri_indices` / counts — faithful to `GXDraw.c:46`
  (numMajor latitude bands, each a `(numMinor+1)*2` triangle-strip; outer-then-inner ring order).
  **VERIFIED** by a new `render_test` unit `imm_sphere` (counts, unit-sphere, north-pole + first-ring
  spec positions, per-band index spans). render_test now 11/11.
- `ngx_emit_imm_sphere` (ngx_j3d_shape.cpp): rebuilds the dome in clip space (PNMTX0 + projection),
  flat PASSCLR = captured matColor, sky PE state (z-test LEQUAL + z-write, opaque ONE/ZERO, CULL_FRONT).
- Capture in `imm_geom_native.cpp`: GXDrawSphere @ 0x80362268 (numMajor=r3, numMinor=r4) +
  GXSetChanMatColor @ 0x8035f51c. The matColor ABI is **VERIFIED by disasm**: `lbz r,0..2(r4)` ⇒ GXColor
  passed BY POINTER in r4 (R@0,G@1,B@2,A@3) — see memory `gx-color-args-by-pointer`. Removed the old
  duplicate debug stub at 0x80362268 in efb_readback_native.cpp.

## The hard-rule wall (this is the real finding)
**GXDrawSphere fires 0 times in EVERY reachable scene** — fastboot plaza, title, AND file-select (DBG_EFB,
direct grep; GXDrawCube @ 0x803627fc fires fine right next to it, so the hook is correct — it's simply
not called). The shipping game uses textured `sky.bmd` skies everywhere reachable; the GXDrawSphere dome
is unused in these scenes. So:
- The oracle DISPROVED my hypothesis: the plaza sky-region delta is NOT a missing sphere (the GX oracle's
  top region is greenish terrain/textured-sky, not the dome's blue `0,12,EE`). The dominant plaza deltas
  are the floor center/bottom = the **PARKED wash**, not the sky.
- Per the TOOLING/VERIFICATION-FIRST hard rule ("Do not port effects you cannot verify"), I CANNOT
  declare this port done — there is no reachable scene to verify the end-to-end render.

## Status of the GXDrawSphere change (committed, but honestly labeled)
- VERIFIED: geometry (`render_test imm_sphere`), matColor ABI (disasm).
- UNVERIFIED: the end-to-end dome render (no reachable scene calls GXDrawSphere).
- CONFIRMED INERT / NO REGRESSION: with the change in, plaza ab_oracle is still 18.4% (unchanged) —
  the capture is gated on ngx-present AND only fires on an actual GXDrawSphere call, of which there are
  none in reachable scenes. So it cannot regress any currently-working scene; at worst it would render a
  dome (right or wrong) in a scene that is currently *also* missing it.
- PARKED pending a scene that actually draws the sky dome (unknown which stage; `mMap==15` only adds the
  rotation, not the draw). Don't re-assert "sky sphere = the plaza sky gap" — it is NOT (falsified here).

Lesson reinforced: check effect REACHABILITY (does the call even fire in a scene I can reach?) BEFORE
porting — that is the reachability half of the tooling-first rule, and it would have caught this earlier.

---

# 2026-06-19 (cont.) — /savestate + the reachability wall (effect verification is gated on scene-reaching)

Continuing, I built the rest of the drive→save→load→verify loop and probed the next verifiable effects.
The consistent finding: **every reachable-now divergence is the PARKED wash; the non-parked effects
don't fire in any state I can produce.**

## Shipped (committed 7559c84)
- **`/savestate?f=<path>`** probe endpoint (symmetric with `/loadstate`, `State::SaveAs` via
  `Core::RunOnCPUThread`). VERIFIED round-trip: writes 27 MB on demand, `/loadstate` reloads it, process
  stays alive. Completes the `/loadstate` + `/savestate` + `/pad` scripted loop.

## Reachability findings (all checked on real data — these are the value of this round)
- **Plaza fog is OFF**: `/gxstate` GXSetFog TEE = `type=0 (GX_FOG_NONE)`, fsel=0 (called 6940× but always
  NONE — gd-reinit-gx disables it each frame). So the plaza center over-brightness is NOT fog; porting GX
  fog would be another unreachable-effect trap. (ngx captures fog state but doesn't apply it — correct to
  leave unmodeled until a fog-active scene is reachable.)
- **Sun occlusion is dormant, sun off-screen**: `TSunMgr::drawSyncCallback` fires (unk14=1, sun model
  loaded) but `TSunModel::getZBufValue` (sunmodel.cpp:265) runs GXPeekZ only for sample points that
  project ON-screen (`it->x != -1`); the sun is off-screen in fastboot plaza so all 17 points are
  (-1,-1) → GXPeekZ fires 0×. The native GXPeekZ override (`ov_gxpeekz`, reads ngx depth) is BUILT and
  correct but can't be exercised until the sun is in view. C-stick combos (cup/cdown/cleft/cright) exist
  for camera aiming.
- **fastboot Delfino = the Entrance.thp cutscene, NOT controllable free-roam**: drove RIGHT 3 s from the
  loaded state → before/after screenshots byte-identical (md5 match) → Mario isn't controllable there.
  `scratch/fresh_plaza.sav` (emu 70 s) and `scratch/freeroam_plaza.sav` are both in/near the entrance;
  neither is a free-roam camera-drivable state. (Don't rely on them for camera driving.)

## Where this leaves it (the honest next frontier)
The verification TOOLING is complete and proven (oracle / loadstate / savestate / ab_oracle.sh). USING it
to verify NEW effects is gated on **game-state reachability**: get to a controllable free-roam scene
(past the entrance cutscene), aim the camera to bring the sun into view (→ GXPeekZ activates → verify the
native sun-occlusion vs the Dolphin-GX baseline), and/or transition into a fog-active level. That is a
separate, non-trivial driving/RE task (and may be easier from a headed session where the user can drive
to a good spot and `/savestate` it). Per the tooling-first hard rule I did NOT port further effects I
can't verify. The reachable plaza divergence remains the PARKED wash — not chasing it per the directive.
