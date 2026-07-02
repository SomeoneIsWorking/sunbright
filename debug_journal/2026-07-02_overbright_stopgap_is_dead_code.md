# Title-screen overbright: the ph6-MapXlu stopgap is dead code (2026-07-02)

## What was true then (debug_journal/2026-06-30, session N+4)
Baseline `title_overbright.py` (was `fileselect_overbright.py`) mean|Δ| = **42.6**. Cause = the
phase-6 (mPerformListGXPost) re-flush of `DrawBuf MapXlu` painted the tev=3 sea-MASK packet (batch
key `eb5c8e74`, material `c97c48`, SRCALPHA/SRCCLR) as a full-screen opaque-white overdraw. Stopgap
`SB_KEEP_PH6_MAPXLU` (unset = suppress ph6 MapXlu draw) → 14.0 (verified in reference/sms/src/JSystem/
JDrama/JDRDrawBufObj.cpp:77-99).

## What's true now (2026-07-02, HEAD 61fd71e)
Same tool, same measurement protocol, same fresh Dolphin-GX oracle PNG (SUNBRIGHT_STAGE=15 headless
fastboot to APP_STATE_GAMEPLAY title screen, no input): mean|Δ| = **58.2**. Measured identically
across five checked commits (65cd337 session 15, 2b3bfd1 pre-lighting, 927d8a5 mid-July-01, 32a03fa
Mario mangled fix): **all measure 58.2** (identical to 3 decimal places → the boot-timed frame
329 is deterministic and every recent commit lands the SAME pixels). So there was NO single-commit
regression flipping 42.6 → 58.2 — either the number was always this on this measurement path, or
the earlier 42.6 was measured on a different visual state (e.g. post-Start pressed into the
file-blocks sub-screen).

### Per-phase ablation sweep (SB_ABLATE_PHASE=N)
| Phase | Δ | Δ vs 58.2 |
| ---   | --- | ---     |
|   1   | 44.7 | −13.5 |
|   2   | 58.2 |   0   |
|   3   | 58.2 |   0   |
|   4   | 45.5 | −12.7 |
|   5   | 58.2 |   0   |
|   6   | 57.5 |  −0.7 |

Dropping ph1 or ph4 individually recovers ~13 of the 58.2. Dropping both jointly (SB_ABLATE_PHASE=1,4)
sends the frame nearly black (delta flips negative, absolute 53.2) — those two phases together are
essentially the whole scene. So the overbright is a genuine ph1+ph4 **double-draw**, not localized
to a single phase.

### ph6 has moved on — the stopgap is dead code
`SB_DRAWFLAG_DBG=1` captures every phase=N + buffer-name pair that hits `drawHead`. **`DrawBuf
MapXlu` fires at phase=0 (frameInit setup) and phase=1 (unk40 drawBufferGroup flush) only.** In
phase=6 the buffers are `DrawBuf ChrOpa`, `DrawBuf ChrXlu`, `DrawBuf Map 半透明優先 (opa)`, `DrawBuf
Map 半透明優先 (xlu)`, `DrawBuf Map 半透明優先2 (opa)`, `DrawBuf Map 半透明優先2 (xlu)`, `DrawBuf
StaticMapObj SunXlu`, `DrawBuf StaticMapObj ShadowXlu`, `DrawBuf Indirect`, `DrawBuf AfterIndirect
Opa/Xlu`, `DrawBuf LensFlare`. **No `DrawBuf MapXlu` in phase 6 anywhere.**

The stopgap in `reference/sms/src/JSystem/JDrama/JDRDrawBufObj.cpp:92-99` gates
`phase==6 && strcmp(name,"DrawBuf MapXlu")==0` → **never true** → the `if (!keep) return;` never
runs → the stopgap is a no-op. Corroborated by direct measurement: `SB_KEEP_PH6_MAPXLU=1` (stopgap
disabled) and unset (stopgap enabled) BOTH measure 58.2 — the env var has no effect. The
`SB_ABLATE_PHASE=6` measurement (Δ=57.5, movement −0.7) is consistent with ph6 no longer being where
the wash lives.

### The key `eb5c8e74` no longer owns the wash
`SB_SKIP_KEY=eb5c8e74` (the mask-material key skip used by the journal, dropped the metric to 14.2
at that time) → 58.2 today, no movement. That specific shader-key packet is either no longer
captured with that key (native's batch coalescer changed) or no longer produces the visible wash.

## Interpretation
The whole **class** of overbright (ph1+ph4 double-draw of the scene that includes tev=3 SRCALPHA/SRCCLR
mask draws) is STILL the source, but the specific batch identity (buffer name + shader key) targeted
by the stopgap and by `SB_SKIP_KEY=eb5c8e74` has shifted. Likely causes (not chased further this
session — see next):
- The PerformLists.bin routing places the sea-mask material in `DrawBuf Map 半透明優先` buffers now
  (not `DrawBuf MapXlu`).
- The J3D-capture batch coalescer's shader-key hash changed between June and July (rebasing on
  session 13's lighting ports would move keys — TLightCommon::setLight now loads 3 lights that
  affect material keys).
- The debug_journal's `boot_0329.ppm` was a different visual state (post-Start file-blocks vs
  pre-Start PUSH START title).

## Actions (this session)
- **Delete the dead stopgap** in `reference/sms/src/JSystem/JDrama/JDRDrawBufObj.cpp:77-99`. It's
  dead code that misleads future sessions into thinking the wash is gated when it isn't. The
  bandaid was already documented as a stopgap (the journal's "PROPER FIX: port the water-reflection
  MapXlu re-entry"), and it's no-op now anyway.
- Document this finding here so next session doesn't chase 42.6→58.2 as a regression.

## Bandaid-free fix (unchanged, well-scoped)
The real fix remains what debug_journal/2026-06-30_fileselect_overbright_is_efb_target_structure.md
already scopes: reproduce GC's EFB-copy-texture composite. `TMapObjSeaIndirect::perform` at
`reference/sms/src/MoveBG/MapObjWater.cpp:26` is still an EMPTY STUB — its `init()` already binds
`スクリーンテクスチャ` at texmap slot 1 for `/common/map/UNDERwater.bmd`, so `perform` needs to
draw the reflective sea sampling that EFB screen texture.
