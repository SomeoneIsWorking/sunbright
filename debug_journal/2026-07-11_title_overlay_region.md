# 2026-07-11 — Title "white haze" defect ROOT CAUSE: JP-vs-US overlay-pane count mismatch

This is the **real** title defect the user reported ("not exactly faithful, has problems
overall"). It is distinct from the prior session's "washed logo" chase (which was the
dump-BGRA-mislabel, resolved in `2026-07-11_dump_bgra_mislabel.md`). With the dump path fixed,
the visible defect became measurable: a **semi-transparent white haze** concentrated in the
mid-band ("SUNSHINE" + PRESS START region), delta `[57,42,28]` brighter than the oracle there.

## ROOT CAUSE: JP code drives 13 overlay panes; the US `.blo` defines 18

The title logo assembles from `p_0X` fly-in overlay letter panes that fade out once the logo
holds. The JP decomp (the port's source) sizes `unk1D4[13]` and loops `i < 13` in
`titleDraw`. The US disc's title `.blo` defines **`p_01`..`p_18`** (18 panes — confirmed by
`SB_J2D_DRAW_DUMP`: 18 distinct `p_` tags drawn). So `p_14`..`p_18` are created by the `.blo`
and rendered by the J2D screen traversal at their default alpha (255), but **native's game
logic never creates `TExPane` wrappers for them and never drives their fade** → they stay
visible forever → the white haze.

The function-size delta predicted this before any disasm: **JP `titleDraw` = 0x948 (2376 B);
US = 2888 B; PAL = 0xB50 (2896 B).** JP genuinely has fewer panes (13); US/PAL have 18.

## Disasm verification (US DOL, `scratch/sms_us.dol` @ `0x8016c060`)

Three independent checks all confirm US = 18:
1. **Case-3 loop bound:** `0x8016c380: cmpwi r20,0x12` → 18 iterations (0x12 = 18).
2. **Case-1 completion trigger:** `0x8016c1a0: cmpwi r17,0x11` → fires on `i == 17` (the 18th).
3. **Completion unroll:** `0x8016c1ac: li r0,0x2; mtspr CTR,r0` → CTR=2 over a 9-pane unrolled
   body = 18 panes written with the fade-in setup (offsets 0x1d4..0x1f0 + `+500` stride).
4. **Array base** is `0x1d4` in BOTH JP and US (same offset) — only the *count* differs.

The fade constants (case-1 in: 180.0/+1.875; case-3 out: 255.0/-1.8214285) are region-identical
(already verified in `2026-07-11_dump_bgra_mislabel.md`). Only the iteration count differs.

## Evidence the panes were the defect (A/B)

`SB_SKIP_MBLACK=ffffff00` (skip all overlay panes with steady-state mBlack): mid-band dropped
from `[179.8, 200.2, 219.6]` (white haze) → `[120.8, 158.1, 194.1]` ≈ oracle `[127, 160, 193]`.
And `SB_J2D_DRAW_DUMP` showed `p_01`..`p_13` at `mAlpha=0` (correctly faded) but
`p_14`..`p_18` stuck at `mAlpha=255` (never driven).

## The fix (region-aware, not a hardcoded count)

Per "no bandaids — fix the cause": the cause is that the pane count is a compile-time
constant (13) that's wrong for the US/PAL `.blo`. The fix makes it runtime + data-driven:

1. **Renamed the title fields** (RE'd from the `.blo` pane tags + titleDraw logic +
   `TCardLoad::load`): `unk18`→`mTitleAnimState`, `unkF0`→`mTitleLogoPane` (`'titl'`),
   `unkF4`→`mTitleCopyrightPane` (`'nint'`), `unkF8`→`mTitleSparkles[11]` (`'s_0X'`),
   `unk1D4`→`mTitleOverlayPanes[18]` (`'p_0X'`), `unk248`/`unk22E`→`mOverlayAnimState`/
   `mOverlayDelay`, `unk222`/`unk20C`→`mSparkleAnimState`/`mSparkleTimer`, `unk124`→
   `mSparkleInitialBounds`, `unk258`→`mTitleStepCounter`. (NOT `unkBC`/`bm->unk18`/
   `unk40[].unk18` — those belong to `TCardBookmarkInfo`, a different struct, and were
   left alone.) NOTE: other classes (SunModel, BathtubKiller, Conductor, Enemy) have their
   own unrelated position-based `unkF0`/`unk1D4` members — unaffected.

2. **Resized the overlay arrays** `mTitleOverlayPanes`/`mOverlayDelay`/`mOverlayAnimState`
   from `[13]` to `[18]` (the max). Safe: all field access on native is via named members,
   never raw GC byte offsets (verified — the GC offsets in comments are documentation only;
   the LP64 build's layout already diverges from GC). The sparkle arrays stay `[11]` (11
   sparkles in all regions: s_01..s_11).

3. **Added `mTitleOverlayCount` (u8)**, set at `TCardLoad::load` time by creating `TExPane`
   wrappers for `p_01`..`p_18` and stopping at the first `search()` miss (the `.blo`'s `p_`
   run is contiguous). `TExPane` already tolerates a null pane (region-tolerant ctor), so the
   probe is harmless. Result: 13 on a JP ROM, 18 on US/PAL — **data-driven, no region table.**

4. **Changed every titleDraw overlay loop** (`case 1/2/3` + the two `perform()` "skip to
   PRESS START" handlers + the init loop) to iterate `i < mTitleOverlayCount`, and the
   case-1 completion trigger from `i == 12` to `i == mTitleOverlayCount - 1` (matching
   retail's `i == 0x11` on US, `i == 12` on JP).

## Verification

- **Build clean.**
- **Alpha trace** (`SB_J2D_DRAW_DUMP`): `p_14`..`p_18` now reach `mAlpha=0 mColorAlpha=0` at
  the hold (were `mAlpha=255`). `p_01`/`p_13` unchanged (still correctly fade).
- **Pixel**: bottom-center region residual vs phase-matched oracle dropped **87.0 → 4.2**;
  mid-band `[179.8,200.2,219.6]` (haze) → `[141.3,174.4,209.7]` (oracle `[121,157,193]`).
  The white haze is gone. (Residual mid-band ~20 brighter than oracle is the pre-existing
  minor global brightness difference noted in the dump-mislabel journal, not this bug.)

## Region handling summary

| region | overlay panes (`p_0X`) | sparkle panes (`s_0X`) | titleDraw fn size |
|---|---|---|---|
| JP (GMSJ01) | 13 | 11 | 0x948 |
| US (GMSE01) | 18 | 11 | 2888 |
| PAL (GMSP01)| 18 (by size analogy) | 11 | 0xB50 |

The runtime `.blo` probe handles all three (and any future region) without a hardcoded table.

## Lesson

The prior multi-session "washed logo" investigation kept probing *render* hypotheses
(blend, TEV, EFB snapshot, duotone) when the real cause was a **region-count mismatch in
the game logic** — a data/code version skew between the JP decomp source and the US assets.
The function-size delta (JP 2376 vs US 2888) was an early, free signal that the regions'
titleDraw differ structurally; it should have been the first thing checked once two regions
were in play. **When running JP decomp code against a US ROM, any per-element array/loop with
a hardcoded count is a suspect for a region mismatch** — verify against the US binary's
disasm, don't assume the JP constant is universal.
