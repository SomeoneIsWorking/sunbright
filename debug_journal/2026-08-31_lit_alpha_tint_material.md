# 2026-08-31 — lit alpha-tint hand material

The watched recomp title audit identified a 52-draw perspective-reached family formerly rejected
as `c008f82fc108f2f0` (`_mat_hand3_L`/unnamed materials). Decoding the authored J3D stage bytes
through the Aurora/Dolphin BP layout gives:

- color: `A=ZERO, B=RASC, C=C0, D=ZERO`, so the result is primary diffuse raster multiplied by
  TEV register-0 RGB;
- alpha: `A=ZERO, B=TEXA, C=RASA, D=ZERO`, so the result is texture alpha multiplied by material
  alpha;
- the stage is one texture at coordinate/map 0, `COLOR0A0`, with ordinary opaque depth-write
  policy and pass-all alpha comparison.

This is not the existing texture-times-diffuse family because texture RGB is deliberately ignored,
and it is not the solid-colour mask because its RGB is lighting multiplied by a dynamic authored
tint. `native-render` now owns this as `LitAlphaTintMaterial` with a dedicated alpha-only shader;
both recomp and decomp adapters use the same exact classifier. CPU controls cover stage mutation,
missing normal/light context, and alpha-policy mutation.

The guarded 120-present audit after the change reported 52/52 classification, resource decode,
scene readiness, and model submissions for this family. Total semantic coverage rose from 1,404
to 1,456 models and lit coverage from 1,300 to 1,352. Exit status was 0 and the live GPU watcher
reported no kernel GPU fault or reset.
