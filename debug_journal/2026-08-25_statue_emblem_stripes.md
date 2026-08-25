# 2026-08-25 — statue emblem stripes: what is ruled out, and the next instrument

## The defect (user-reported, reproduced)

The Delfino Plaza statue's pollution-graffiti glyph renders with regular VERTICAL STRIPES
(~6px-period green/pink chroma bands, identical row-to-row) when the camera is CLOSE. Far away the
same glyph is a smooth gradient. Reproduced deterministically:

```
SBR_FASTBOOT=1 SB_HEADLESS=1 SBR_QUIT_AFTER=4000 \
SBR_PAD_SCRIPT="400:STICK=0/100,1100:STICK=0/0" ./run-recomp.sh
```
(walk forward ~10s from plaza spawn; the statue is dead ahead). Retail ground truth
(`scratch/screenshots/dolphin_fastboot_plaza.png`, Dolphin GX at the same spot) shows the glyph as
a SMOOTH rainbow-ish gradient with no stripes at that distance.

## Established (each by measurement, not argument)

1. **The rainbow look itself is CORRECT.** Retail's emblem is a shimmering rainbow glyph; our far
   view matches its palette and layout. Only the stripes are the defect.
2. **The glyph is the pollution-graffiti decal** (green goop + pink drips), drawn into an EFB-copy
   canvas: a double-buffered 64x128 RGB5A3 pair appears in guest RAM per run (e.g. 0x80e22720 /
   0x80e2d0a0), and its RAM copy is STALE NOISE — the live canvas is GPU-side only.
3. **NOT the indirect warp.** A/B with every indirect stage disabled (temporary `SB_DBG_IND_OFF`
   build, shader cache cleared because the pipeline cache keys on config, not source) — the
   stripes are IDENTICAL. The emblem's indirect config is ITF_8 / bias STU / ITM_0 / wraps OFF;
   the matrix math in aurora matches Dolphin's `idot >> 3` + `2^(17-scale)` exactly (verified
   line-by-line against PixelShaderGen.cpp + PixelShaderManager.cpp). Diagnostic reverted.
4. **NOT static texture content.** All 217 static textures were fetched via the probe server and
   decoded (GX I4/I8/IA4/IA8/RGB565/RGB5A3/CMPR): none is a rainbow ramp, none has vertical-stripe
   structure, none changes bytes over time. The remaining string-compare cost in the profile is
   the GUEST's own strncmp (faithful game code), not our state.
5. **The stripes are chroma-only** (G-R oscillation ~±90 per ~6px; luminance nearly constant),
   stable across rows, present only in the fine-detail regions of the art.

## Prime suspect (untested)

The graffiti CANVAS pipeline: the game renders the art into an EFB region and copies it to the
canvas texture; the decal samples that. A copy-region/stride/format mismatch on OUR side would
resample the art at the wrong pitch — horizontal compression of drippy art reads as vertical
banding. The `copydbg` channel printed NOTHING during a plaza run even though the canvas must be
copied — either the channel is not reached on the recomp lane's copy path, or the copy goes
through a route with no logging. That silence is itself the next defect to fix: a diagnostic that
cannot see the copy it exists to log.

## Next steps, in order

1. Fix/verify copydbg on the recomp lane; capture the graffiti copy's rect, size and format.
2. Compare the copy rect against the canvas texture object's declared size — a mismatch is the
   stripe mechanism.
3. Ground truth at the CLOSE camera: build the retired Dolphin-backed oracle
   (`tools/oracle/build_dolphin_fastboot.sh`, rev 9283f44^) and screenshot the same spot, so
   "retail smooth vs ours striped" is measured at the SAME camera, not inferred across distances.

## Instruments built along the way (reusable)

- Probe-server texture sweep: `SBR_LUCENT_DEBUG=texresolve SBR_PROBE=1` + the fetch/decode script
  pattern in this journal's session — decodes every static GXTexObj from guest RAM per its logged
  format (nibble handling for I4/IA4 was wrong in the first attempt: 4-bit formats carry TWO texels
  per byte, high nibble FIRST).
- `SB_SHADER_DUMP=1` prints the full TEV+IND config per distinct shader — the emblem's indirect
  stage was identified this way (32 shaders with numIndStages>0 in a plaza run).
- Pad-script walking (`SBR_PAD_SCRIPT="400:STICK=0/100,1100:STICK=0/0"`) reaches the statue from
  plaza spawn deterministically.
