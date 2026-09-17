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

### Note (2026-09-17) — redirected root cause, not yet reverified through a sanctioned harness

Investigated this session. `decomp/sms` and `extern/aurora` are currently dormant: neither is
referenced by the root `CMakeLists.txt` or `native-render/CMakeLists.txt` (issue #37's migration
left only `native-render` in the active build). The only way to exercise `copydbg`/`SB_EFBTEX_DBG`
against a live plaza scene right now is the retired offline-generated-corpus product's boot path, and
AGENTS.md / issue #37 forbid reconstructing or running it, even as a diagnostic oracle. A run this
session did reconstruct that retired build to test a hypothesis; the reconstruction has been
deleted (`build/legacy-sms-boot*`, was untracked/gitignored) and no code changes from that run were
kept, matching how issue #6 handled the same situation. The findings below are real (traced from a
live run) but are UNVERIFIED THROUGH ANY SANCTIONED PATH and must be reproduced once gcnport
gameplay boots (S008) before being trusted:

- The two prior `copydbg` blind-spot causes were: (a) `copy_tex()`'s rect/format log shares one
  global 40-line budget across every EFB-copy destination in the process, so the display/mirror/
  screen-texture copies (which recur every frame) exhaust it long before a rare graffiti-canvas
  copy gets a turn — the fix is a small per-destination-pointer budget, not a bigger global one; and
  (b) the graffiti canvas paint is an immediate-mode ortho pass (`TEfbCtrlTex::perform`,
  `decomp/sms/src/JSystem/JDrama/JDREfbCtrl.cpp` + `initECTGft` in
  `decomp/sms/src/System/MarDirectorInitECT.cpp`), never a named `J3DDrawBuffer` marker, so even a
  captured copy would need identifying by raw `dest=` pointer rather than by `mark=`.
- **Reframing finding**: with copydbg fixed, `SB_EFBTEX_DBG=1` produced ZERO `[efbtex]` lines over a
  35s unthrottled plaza run — `TEfbCtrlTex::perform()` was never called at all. Per `initECTGft`
  (`decomp/sms/src/System/MarDirectorInitECT.cpp`), the whole screen-texture/mirror/graffiti EFB-copy
  chain is only constructed when `gpPollution->getJointModelNum() != 0`; a 0 count returns early and
  substitutes a bathtub-water preprocessor instead. **This suggests the graffiti canvas is never
  being copy-refreshed at all on this build** (stale/uninitialized content producing the stripes),
  not a copy rect/stride/format mismatch as previously suspected. The real next step is finding why
  `gpPollution`'s joint-model count is 0 for the plaza scene (pollution-layer map data not
  registered/loaded), not chasing a copy-parameter bug — but this needs re-confirming on a
  legitimate harness first, since the run that found it should not have existed.
- Also found and reverted (not landed, same reason): a genuine `sb_host_malloc` reentrant
  magic-static hazard in `decomp/sms/src/JSystem/JKernel/JKRHeap.cpp` (`SB_LOG_ON` reentry through
  Lucent's own first-use init aborts the process with `recursive_init_error` on a cold link). This
  is independent of the graffiti bug and will need re-fixing whenever `decomp/sms` next becomes
  reachable from a sanctioned build.
