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
