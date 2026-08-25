---
id: 11
title: the statue graffiti glyph renders with vertical stripes up close (recomp + Aurora)
status: open
symptom: Delfino Plaza statue emblem shows ~6px-period vertical green/pink chroma stripes at close camera; smooth gradient at distance; retail (Dolphin) is smooth
tags: render,gpu,graffiti,efb-copy
created: 2026-08-25
updated: 2026-08-25
---

## What happens

Walk from plaza spawn toward the statue. The pollution-graffiti glyph on its face renders with
regular vertical stripes up close; far away it is a smooth rainbow gradient (which matches retail's
holographic look — the rainbow itself is correct).

## Ruled out (2026-08-25, see debug_journal/2026-08-25_statue_emblem_stripes.md)

- Indirect texturing: A/B with all indirect stages disabled reproduces identical stripes; aurora's
  ITM math verified line-by-line against Dolphin.
- Static texture content: all 217 static textures decoded from guest RAM — no rainbow ramp, no
  stripe structure, no per-frame byte changes.
- Geometry/UVs: the glyph silhouette is correct in every capture.

## Prime suspect

The graffiti EFB-copy canvas pipeline (double-buffered 64x128 RGB5A3 pair, GPU-side; RAM copies are
stale noise). A copy rect/stride/format mismatch resamples the art at the wrong pitch and reads as
vertical banding. The `copydbg` diagnostic printed NOTHING for the canvas copies on the recomp
lane — that blind spot must be fixed first.

## Next step when picked up

1. Make copydbg reach the recomp lane's copy path; log the graffiti copy rect/size/format.
2. Diff the copy rect against the canvas texture object's declared size.
3. Build the Dolphin-backed oracle (tools/oracle/build_dolphin_fastboot.sh) for a same-camera
   retail close-up.
