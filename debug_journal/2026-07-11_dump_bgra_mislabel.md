# 2026-07-11 — ★★★ ROOT CAUSE of the multi-session "title color" defect: SB_DUMP_FRAME writes BGRA8 but labels it RGBA

This single finding **falsifies the entire "washed / orange / blue-diluted logo" line of
investigation** that spans `2026-07-10_title_pixel_diagnosis.md` and several prior sessions.
The native title render is FAITHFUL. The defect was in the **diagnostic dump path**, not the
renderer.

## The trap

`SB_DUMP_FRAME` (aurora `lib/aurora.cpp`, `end_frame` lambda) does `CopyTextureToBuffer` from
the present-source texture into a mapped readback buffer and writes the raw bytes to disk. The
present-source texture is in the **host surface format**, which on this machine (Vulkan) is
**`BGRA8Unorm`** (confirmed in the startup log: `Using surface format BGRA8Unorm`). So the
bytes on disk are `B,G,R,A` per pixel.

But the code LOGGED `wrote {}x{} RGBA to {}` — and every downstream consumer (PIL
`Image.frombytes('RGBA', ...)`, ImageMagick `rgba:`, eyeballing) read those bytes as
`R,G,B,A`. **Reading BGRA bytes as RGBA swaps the red and blue channels.** A blue sky becomes
orange; blue logo letters become orange/red; the whole frame takes a warm cast.

Aurora's own comment (lines 287-292 before this fix) already documented the trap:
> "using rgba: swaps red/blue and has already caused one false 'wrong colors' diagnosis — the
> render was correct, the conversion wasn't."

That warning was not enforced in code, and the trap fired again — for **multiple sessions**,
producing the "title logo is washed/blue-diluted/oversized" narrative that consumed enormous
investigation effort (J2D bounds, duotone overlays, EFB snapshot composites, mBlack bisects,
titleDraw state-machine diffs). All of those measurements were taken on BGRA-misread dumps.

## Proof

Same dump bytes, two readings (`scratch/screenshots/sw_700.rgba`, native title at 700 presents,
surface format BGRA8Unorm):

| reading | sky top-15% mean RGB | R/B ratio |
|---|---|---|
| **as RGBA (WRONG — what every prior session did)** | `[236.3, 206.9, 169.8]` (orange) | 1.39 |
| **as BGRA→RGBA (CORRECT)** | `[169.8, 206.9, 236.3]` (blue) | 0.72 |
| oracle `check_3800.png` sky | `[170.1, 204.7, 230.4]` (blue) | 0.74 |

BGRA-corrected native sky `[169.8, 206.9, 236.3]` ≈ oracle `[170.1, 204.7, 230.4]` — a near
match. Logo band: native `[120.6, 161.2, 232.1]` vs oracle `[110.7, 158.2, 225.7]` — near
match (native very slightly brighter overall; a small residual, not a hue bug). Swapping R/B on
the oracle (the inverse operation) reproduces the native "orange" look exactly, confirming the
swap is the whole story.

## The fix (landed this session)

`extern/aurora/lib/aurora.cpp`: `SB_DUMP_FRAME` now **normalizes its output to true RGBA8**
regardless of the host surface format. At queue time it records
`s_dumpSwapRB = (surfaceFormat == BGRA8Unorm)`; at write time it swaps bytes 0↔2 of each pixel
when that flag is set, so the file always matches its "RGBA" label and any standard RGBA reader
is correct by default. The misleading log + comment are corrected. Verified: a fresh dump read
directly as RGBA (no manual swap) yields blue-correct sky `[169.8, 206.9, 236.3]` ≈ oracle.

This is a **workflow-first fix**: the dump path is the primary verification lens for this
project, and a mislabeled dump taxes every future session. The output now honors its contract.

## ALSO confirmed this session: titleDraw is faithful (the overlay-diluter lead is dead)

A full diff of the port's `TCardLoad::titleDraw` (`reference/sms/src/GC2D/CardLoad.cpp`) vs the
US retail decompile (`scratch/decomp/8016c060.c` = GMSE01 `titleDraw`) shows the `unk18` state
machine is FAITHFUL:
- Case 1 completion (fade-in): retail raw-writes `mCurrentAlpha=180.0, mAlphaStep=1.875,
  mTargetAlpha=255` (SDA2[-0x4964]=180.0f, SDA2[-0x4960]=1.875f). Port's `setPaneAlpha(40,255,180)`
  computes the identical `mAlphaStep=(255-180)/40=1.875`. ✓
- Case 3 completion (fade-out to 0): retail raw-writes `mCurrentAlpha=255.0, mAlphaStep=-1.8214285,
  mTargetAlpha=0` (SDA2[-0x4980]=255.0f, SDA2[-0x495c]=-1.8214285f = -(255/140)). Port's
  `setPaneAlpha(140,0,255)` computes the identical step. ✓
- Case 2 (overlay update gated behind `unkF0` alpha saturation): byte-for-byte match including
  the alpha-clamp fix already landed (commit `59c6dcc` era). ✓

And the `SB_TITLE_ALPHA_DBG` trace (temp probe, now removed) showed the `unk1D4` overlay panes
**DO reach alpha 0 at the unk18==4 PRESS START hold** (`overlayVisAlpha(>4)=0` for thousands of
frames at hold). The "p_0X overlay letters don't fade out" premise from the prior session is
**false** — they fade correctly. `mBlack=0xFFFFFF00` is the retail steady-state value too (the
case-3 `r+=7` loop reaches r=255 in both), so it is not a divergence.

## FALSIFIED — do NOT re-chase

Everything in `2026-07-10_title_pixel_diagnosis.md` tagged as a title-render defect that was
measured off a dump is confounded by this mislabel and must be re-validated against
BGRA-corrected (now: default) dumps before being trusted:
- "native sky over-bright/too-blue / blue-white wash" — **false**; sky ≈ oracle once BGRA-corrected.
- "logo blown up / blurry / oversized" — **false** at the settled hold (≥300 presents); was
  mid-animation capture, and the "blur" was the misread.
- "fly-in glyph layers don't fade out → dilute the blue base" — **false**; overlay alpha reaches
  0 at the hold; the measured "dilution" was the BGRA misread making blue look pale.
- "mBlack=0xFFFFFF00 overlay panes are the letter diluter" — **false**; that mBlack value is
  retail-identical steady state, and the panes are invisible (alpha 0) at the hold.
- All J2D-bounds / wrap / binding / duotone / EFB-snapshot-composite hypotheses — measured off
  misread dumps; the render path is faithful.

## What IS a real (minor) residual, if anyone returns to title parity

- Native whole-frame is ~15-25 levels brighter than the oracle (e.g. native logo-band
  `[120,161,232]` vs oracle `[110,158,226]`). Small global brightness/gamma difference, NOT a
  hue bug. Worth one look at the present/EFB gamma path only if title parity is the active goal.
- "SUNSHINE" word renders slightly paler than "SUPER MARIO" in native vs oracle (visual; may be
  animation phase). PRESS START prompt at a different blink/fade phase — animation timing, not
  a render bug.
- These are cosmetic/phase, not the "title is broken" defect that drove the prior work.

## Lesson (workflow)

> **A diagnostic that lies is worse than no diagnostic.** When a verification tool's output
> format contradicts its label, every conclusion built on it is poisoned, and the poison is
> invisible (the image *looks* plausible, just wrong). The dump path is a verification lens —
> its output contract must be self-describing and honest. Normalize at the boundary; never
> push format-correctness onto every consumer. And when a multi-session investigation keeps
> producing "the colors are subtly wrong" across many independent hypotheses, suspect the
> measurement instrument before the subject.
