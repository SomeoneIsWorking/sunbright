# CORRECTION: the pollution port is INERT, TMapObjWave is irrelevant, the Delfino wash is STILL OPEN

2026-06-18 (session after the "pollution PORTED" handoff). This **supersedes the root-cause claim** in
`2026-06-18_delfino_wash_ROOT_CAUSE_shineshadowvolume.md` and the handoff
`scratch/handoff_2026-06-18_pollution_ported_next_tmapobjwave.md`. Those said the wash was SOLVED by
porting `TModelWaterManager::drawShineShadowVolume` (plaza pollution) and the residual was
`TMapObjWave::initDraw`. Both are **falsified by hard data below.** The user confirmed: "wash is
obviously a missing effect" — and then directed: stop chasing the wash, fix everything else first.

## What was tested (all headless, build-freshtest, emu_secs 14, fastboot File 1 Delfino)

1. **TMapObjWave (the handoff's "next" lead) does NOT touch the floor.** Built `SUNBRIGHT_KILL_MAPOBJWAVE=1`
   (no-op `perform__11TMapObjWave` 0x801dcdd8, in shadow_kill_diag.cpp). Killing the wave in the oracle
   changed **only the water band y0–327** (71127 px), the **floor (y>336) by ZERO** — byte-identical.
   At the wave's own pixels ngx is ~150 off the oracle WITH or WITHOUT the wave (Δ~4). Porting it would
   not move the number. TMapObjWave is the plaza WATER, not a floor/ground darkening. DEAD LEAD.

2. **The ngx pollution pass is INERT — `drawShineShadowVolume` is NEVER called.** Added a fire counter
   to the capture tee `ov_shineshadowvol` (`SUNBRIGHT_DBG_POLL=1`, [poll-tee] line). It fires **0 times**
   at emu_secs 14 in BOTH `NGX_PRESENT=1` AND `NGX_PRESENT=0` (the latter with the tee force-marked
   purejit-safe via `ov_poll_dbg_mark`, so g_enabled=false wouldn't hide it). The dispatch mechanism is
   fine (the J3DShape draw seam fires thousands of times the same way). The game simply does not call
   `drawShineShadowVolume` in this save state. Corollary: `SUNBRIGHT_NGX_NOPOLLUTION=1` changes the ngx
   frame by ~nothing (mean 2.5, floor pixel identical). **The whole pollution port does nothing.**

3. Therefore the prior session's "oracle_ab 89→45, floor 250→129" improvement is NOT attributable to
   the pollution port (it can't run). The current baseline is mean delta ~45.6; the worst regions are
   the **center/mid plaza band** (16×16 cells at x240–360,y196–280: oracle ~90 grey vs ngx ~240 cream,
   delta ~440) and the sky corners. The bottom floor (row3) is the BEST region now (~30). The visible
   ngx artifact is a **bright cream splotch in the center of the plaza around Mario**, fading to grey at
   the edges (the journal mistook this for a "sunlit centre matching the oracle" — the A/B proves the
   oracle has NO such splotch; it is uniformly grey).

## The actual mechanism (consistent with all data + the user's "missing effect")
The plaza pollution darkening is an **EFB-readback-gated effect** (CLAUDE.md known gap: "EFB-readback
effects — sun occlusion, pollution coverage, mirror, dash-blur — read an empty EFB" under ngx present).
The guest decides whether/how much to darken the plaza from a coverage value it reads back from the EFB.
Under ngx present the EFB is empty → the guest skips the darkening → ngx plaza stays bright. The
capture-tee replay approach **cannot work** because there is no live call to capture from. (And it isn't
called in the NGX_PRESENT=0 oracle at emu_secs 14 either, so even the oracle's darkening at this exact
frame may come from elsewhere — the oracle plaza IS dark ~85, so SOME darkening applies; its source is
no longer attributed and is OPEN.)

## Status: WASH IS OPEN, parked per user. Do NOT re-assert "pollution = solved".
When the wash is picked back up (AFTER everything else): the right path is own-it-natively WITHOUT a
capture tee — read the pollution/cleanliness state straight from guest RAM (gpModelWaterManager + flag
0x40000) every frame and drive the darkening, OR provide a real EFB-coverage readback so the guest draws
it. First re-confirm WHAT darkens the oracle plaza at the captured frame (drawShineShadowVolume is NOT
called there — find the real source before porting anything).

## Tooling left in place (env-gated, default off — reusable)
- `SUNBRIGHT_KILL_MAPOBJWAVE=1` — no-op the plaza wave (shadow_kill_diag.cpp).
- `SUNBRIGHT_DBG_POLL=1` — [poll-tee] fire counter on drawShineShadowVolume + force-marks it
  purejit-safe + [poll] param dump in ngx_present draw_pollution.
- `SUNBRIGHT_NGX_NOPOLLUTION` / `SUNBRIGHT_KILL_SHINESHADOW` (pre-existing) — both confirmed to do
  nothing here because the effect never runs.
