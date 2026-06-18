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
