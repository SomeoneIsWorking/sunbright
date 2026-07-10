# 2026-07-10 — Title-fidelity arc opens: unkC0 attract-gate fix + first real title pixel diff

## Fix: TCardLoad ctor omitted unkC0 (attract-idle counter) — reference/sms 22a8534b

`TCardLoad::TCardLoad` (src/GC2D/CardLoad.cpp:67) initialized `unkBC` but not `unkC0`
(CardLoad.hpp:87). `unkC0` is the per-tick idle counter that the attract gate reads at
CardLoad.cpp:940: `if (unkC0 / 120.0f > 45.0f) fireStreamingMovie(12 or 9)`. The JKR heap
does not zero allocations, so every rebuilt TCardLoad inherited garbage and the gate fired
within frames → endless attract bounce (movie 12 → teardown → movie 9 → teardown …); the
title never held the PRESS-START card. Same defect shape as the earlier
MActorAnmData::mIncidentalAnmNum and TMapWireManager::mActorMgrNum uninit-ctor fixes.

### Verification (paced pin recipe, SB_STAGE=15, 190 s)

- `scratch/logs/wf_unkc0fix.log` (recipe run) and `wf_unkc0fix_dbg.log`
  (+`SB_SEL_DBG SB_TITLE_PANE_DBG`): first movie fire is movie 12 at cardload tick ~5457 —
  the EXACT designed gate (45 s x 120 ticks/s = 5400, plus the ~56-tick state-10/9 preamble).
  Refires are perfectly periodic at 5456 ticks (5457 → 10913 → 16369). Ticks run at ~120/s
  (2 per present), so the fire lands at ~2730 presents ≈ 45.5 s — retail behavior.
- State telemetry: `mState -1→10→9 →(introChase 600 ticks)→ 3`, holds state 3 for the full
  window, exits `3→10` only at the movie fire with `unk18=4` — the old in-code note
  "the port never appears to reach unk18==4" is FALSIFIED/resolved (comment updated).
- Stable PRESS-START dumps: e.g. `scratch/frames_title/png/nat2_10/11` and `nat2_19/20`.

### Follow-on defect exposed (NOT fixed here): THP attract movie insta-completes

When the gate now legitimately fires at 45 s, the movie "plays" for only ~31–240 movie
frames and tears down (`[movie] STATE_PLAYING saw thp==3 -> decideNextMode` almost
immediately; screen shows flat white/grey — dumps nat2_08/16). Retail plays the attract
movie for minutes. The THP player subsystem is unported/no-op; the title therefore cycles
title(45 s) → blank pseudo-movie(seconds) → title. Named subsystem: THP playback.

## First real title pixel diff (native vs Dolphin oracle)

**Canonical comparison size: 640x480** (the oracle VI dump resolution). Native 1280x960
frames downscaled with `magick -filter Lanczos -resize 640x480`. No crop, offset, or
overscan/aspect correction was needed — both sides are full-frame 4:3; the geometry aligned
as-is (logo occupies the same screen region). Aspect/overscan: no mismatch found.

- Native frame: `scratch/frames_title/png/nat2_19.png` (present ~3450, mid stable window)
  → `scratch/screenshots/native_title_640.png`
- Oracle frame: `scratch/oracle/frames/oracle_vi00003800.png` (stable card window
  oracle_vi00003400–4200)
- Diff image: `scratch/screenshots/title_diff.png`
  (+ side-by-side `title_diff_triptych.png`)
- **AE: 305063 / 307200 px = 99.3% differing (95.8% at -fuzz 5%); RMSE 0.559.**

### Subsystem divergence inventory (ranked by visual impact — subsystems, not pixels)

1. **Title 3D backdrop absent (native background is pure black).** Oracle steady card shows
   the sky/clouds/sun scene (sky.bmd dome + clouds + sun flare streaks) behind the logo.
   Nothing of the title map draws natively at the card. Highest-impact gap: it is ~90% of
   the frame area.
2. **Logo presented in intro-phase variant, not steady-state variant.** Oracle steady logo =
   blue "glass" letters, gold Shine sprite, red sun wheel. Native stable card still shows
   the intro-style red/orange letters, blue starfish Shine, blue sun wheel, plus the large
   intro layout (logo noticeably larger than oracle's). Likely the steady-state logo
   material pass (env/indirect "glass" TEV — NULL-texMap/indirect family) or the
   phase-driven material switch never engages.
3. **THP attract movie playback** (see above) — insta-completing no-op movie replaces the
   retail minutes-long attract reel; between attract cycles this also costs a stage
   teardown/reload every ~50 s.
4. **PRESS / START text styling.** Present on both, but native renders it dark red
   (intro-phase colors) vs oracle pale blue/white; same phase/material family as (2).
5. **Sun-flare light streaks** across the oracle card (part of the backdrop scene, listed
   separately because it is a distinct effect object) — absent natively.
6. **"©2002 NINTENDO" subtitle**: present both sides; native orange glow style vs oracle
   white/blue — again intro-vs-steady phase styling.

House rule respected: every divergence above names a subsystem to port/fix; no pixel
hand-tuning.

## Post-JRenderer-fix pixel diff (superproject eefa112: "JRenderer.cpp was excluded from
## the native build" — the 3D backdrop now renders)

**Pin:** paced boot, `SB_HEADLESS=1 SB_STAGE=15 SB_SCENARIO=0 SB_WATCHDOG_SECS=200`, no
turbo, `SB_DUMP_FRAME=scratch/screenshots/title_native_2000.rgba SB_DUMP_FRAME_AFTER=2000`.
Present ~2000 lands inside the stable PRESS-START hold (oracle_vi 3400-4200 window) —
confirmed visually: logo fully formed, rainbow trail, sun icon, palm tree, and (new since
the fix) a rendered sky/cloud backdrop all present; not the intro zoom.

**Tooling correction found while converting the dump — record before reuse:**
`SB_DUMP_FRAME`'s own log line (`aurora.cpp:384`, "wrote {}x{} RGBA to {}") is **wrong**.
`create_render_texture()` (`gpu.cpp:261`) allocates every render target — including
`g_frameBuffer`/`g_sbDisplayPresent`, i.e. whatever `present_source()` returns and
`SB_DUMP_FRAME` copies from — with `format = g_graphicsConfig.surfaceConfiguration.format`,
and this run negotiated `BGRA8Unorm` (logged explicitly: "Using surface format
BGRA8Unorm"). The dump is BGRA8, not RGBA8; only the log string lies. Converting with
`magick ... rgba:` (the assigned convention, and the file's own top-of-function comment
warns this exact mistake already caused one false "wrong colors" diagnosis) reproduced that
false diagnosis here: it rendered a warm orange/tan sky with swapped letter colors that
does not exist in-engine. Converting the identical bytes with `bgra:` instead — confirmed
first by channel means (native mean R/G/B 163/149/130 under `rgba:` — warm-biased, vs
oracle's 144/177/201 cool-biased; 168/195/214 under `bgra:` — cool-biased, matching oracle's
ordering) then visually (blue sky, gold/red sun icon, blue "SUPER MARIO" letters, all
correct) — matches. **Every future `SB_DUMP_FRAME` read must check the logged surface
format and convert accordingly (`bgra:` was correct for this run); do not trust the dump's
own "wrote ... RGBA" log line.** Not fixed in code this session (out of scope for this
diff-only task) — the mislabeled log line + stale-but-technically-correct top comment
should be reconciled into one true statement next time `aurora.cpp`'s dump path is touched.

- Native (correctly decoded): `scratch/screenshots/title_native_2000_full.png` (1280x960)
  → Lanczos-resized `scratch/screenshots/title_native_2000_640.png` (640x480, canonical).
- Oracle: `scratch/oracle/frames/oracle_vi00003800.png` (unchanged canonical pin).
- Diff: `scratch/screenshots/title_diff_postfix.png`; triptych
  `scratch/screenshots/title_triptych_postfix.png`.
- **AE: 297635 / 307200 px = 96.9% differing (75.1% at -fuzz 5%); RMSE 0.463 (norm).**
  Down from the pre-fix 99.3% raw / 95.8% fuzz5 / 0.559 RMSE — a real but modest aggregate
  move, because raw AE counts any non-identical RGB triple as "different" and the frame is
  now full of gradients/antialiasing that were pure black (trivially "equal" in the old
  diff's flat regions) before. The subsystem-level change is much larger than the AE
  delta shows — see inventory below. (House rule: AE% is not the gate while subsystems are
  still incomplete; it is cited here only as the requested number, not as a completion
  signal.)

### Subsystem divergence inventory, post-fix (ranked by visual impact)

1. **Sky/cloud backdrop material is still wrong, though no longer absent.** Native's
   backdrop is a near-flat, blown-out light-gray/white gradient with only a faint diagonal
   color-block seam (upper right) — no distinguishable cloud puffs, no bird silhouette.
   Oracle is a saturated blue sky with well-defined cumulus clouds and a bird. This reads as
   a TEV/blend problem (backdrop clipping to near-white, cloud texture not actually being
   sampled into the composite) rather than a missing-geometry problem — geometry/placement
   of the dome now appears to be correct (per the JRenderer fix), the material driving it is
   not. Single biggest remaining gap by screen area.
2. **Logo "SUNSHINE" letter fill is missing its live sea-texture material.** Oracle's
   SUNSHINE lettering is filled with a distinct wavy sea-surface texture (a known SMS title
   trick: the letters sample a live EFB capture). Native shows a flat, overexposed
   white/bloom fill instead. This directly correlates with the **known open item "missing
   EFB copies (mirror capture + SnapTime snapshot)"** and the FIFO-derived pass structure
   already on file (`scratch/oracle/MANIFEST.md`'s `title_pass_structure.txt`: mirror pass →
   EFB-copy → world pass → mid-scene EFB-copy feeding a narrow 26-draw uniform block) — that
   mid-scene EFB-copy's consumer is very likely this exact logo-fill material. Not yet
   wired in native.
3. **Copyright line ("©2002 NINTENDO") loses contrast.** Renders near-invisible
   white-on-near-white in native vs clearly legible white-on-blue in oracle. Almost
   certainly a downstream symptom of (1) — the same backdrop overexposure raises local
   luminance under the text — not an independently-broken subsystem.
4. **Sun-flare ray pattern differs.** Both sides have a bright sunburst in the same screen
   region, but the diff shows a speckled diagonal band there — ray angle/intensity mismatch.
   Likely the same material family as (1), lower impact.
5. **Duplicate/ghost blink-prompt text.** Oracle shows one partial fading "PR" (top-left,
   blue — the PRESS START prompt mid-blink). Native shows a similarly faded element top-left
   AND a second one top-right that oracle does not have at all. A second copy of the same
   text at a different screen position is consistent with the **known open item "phase-1
   ghost pass (double-draw under stale ortho)"** — worth a targeted check of whether the
   blink-prompt draw call is being issued twice per frame under two different (one stale)
   ortho matrices.
6. **Bird sprite absent from native sky.** Cosmetic, lowest ranked.
7. **Persistent thin outline-only diff halo around every logo glyph even where fill colors
   now match.** Likely resample/antialiasing-width difference from the Lanczos downscale,
   not a distinct engine bug — flagged for completeness, not asserted as a subsystem defect
   from a single diff.
