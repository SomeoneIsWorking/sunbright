---
id: C095
kind: claim
status: holds
created: 2026-08-31
tags: renderer,j3d,lighting,recomp,decomp
depends: native-render/src/j3d_lit_material.cpp#classify_j3d_lit_textured_material, native-render/src/j3d_specular_material.cpp#classify_j3d_specular_textured_material, native-render/src/j3d_alpha_masked_material.cpp#classify_j3d_alpha_masked_material, native-render/src/j3d_lit_alpha_mask_material.cpp#classify_j3d_lit_alpha_mask_material, native-render/src/model.cpp#transform_vertex, sms-recomp/overrides/semantic_j3d_lighting.cpp#publish_lighting, sms-boot/runtime/native_j3d_lighting.cpp#sb_native_j3d_publish_stage_lighting, decomp/sms/src/MarioUtil/LightUtil.cpp#TLightCommon::setLight
reconfirmed: 2026-08-31
verified_at: 2026-08-31 10:42:13+00:00
---

## Claim

Both runtimes feed high-level stage ambient, point lights, directional-specular direction and shininess, decoded normals, and exact single-texture diffuse/specular, solid-colour mask, or diffuse-plus-independent-alpha-mask material values into the shared PC-native J3D renderer without consuming GX light state.

## Evidence

Focused CPU controls passed; a guarded 60-present recomp Delfino audit published 400/400 high-level light updates and submitted 60 lit models among 2,900 total; a guarded 400-present native-decomp Delfino audit submitted 36 lit models among 36,948 total; both runs exited 0 under the live GPU watcher.

## What would falsify it

Either adapter reads GX/XF/FIFO light state, the source-native and recomp high-level light inputs no longer classify the same exact material families, the focused lighting controls fail, a reached guarded Delfino run submits zero lit models, an altered specular channel/program is accepted, or an original J3D/setLight body stops executing.

## Re-confirmed 2026-08-31 — diffuse-lit baseline

Focused CPU material/model controls passed; a guarded 60-present recomp Delfino audit published 400/400 high-level light updates and submitted 100 lit models among 3,260 total; a guarded 400-present native-decomp Delfino audit submitted 630 lit models among 41,997 total; both exited 0 under the live GPU watcher. The pass-all alpha-policy control accepts the equivalent AND/OR/XNOR encodings and rejects XOR.

## Re-confirmed 2026-08-31 — directional specular

Exact channel, tint-selector, stage, normal, and shininess controls passed. The watched shipping-shader control distinguished texture-times-diffuse from the affine red-tint result while preserving green through the exact sRGB sample/output conversion, with no kernel GPU fault. A guarded 60-present recomp Delfino audit published 400/400 high-level light updates with zero shininess failures and submitted all 60 perspective-reached `_mat_hand3_L` specular models, raising the lit count to 160 among 3,320 total. A guarded 400-present native-decomp audit raised the lit count to 666 among 42,033 total. Both exited 0 under the live GPU watcher.

## Re-confirmed 2026-08-31 — solid-colour texture mask

The next reached J3D program was decoded before implementation: its RGB equation is the diffuse
channel multiplied by colour register 0, which was a stable `(0,0,0,255)`, while texture alpha is
amplified fourfold before a half-opacity cutout. Exact CPU controls reject an altered register,
stage, normal, or raster policy. A watched shipping-GPU control rejects source alpha 31 and accepts
32 after the 4x scale, with no kernel fault. The guarded 60-present recomp run submitted all 40/40
perspective-reached instances as solid-colour masks, raising native model coverage from 3,320 to
3,360. The guarded 400-present native-decomp run remained healthy at 42,033 models but encountered
zero instances of this exact family; it is therefore not evidence that the decomp scene reached the
new classifier.

## Re-confirmed 2026-08-31 — independently sampled colour and alpha mask

The reached two-stage Mario hand program was decoded as an ordinary high-level equation: its first
texture supplies RGB multiplied by diffuse stage lighting; its second texture supplies alpha only,
through texture-coordinate set 1, with a fourfold scale before the half-opacity cutout. Exact CPU
controls reject changed stage bytes, register alpha, either texture binding, the second coordinate,
normal, lighting, or raster policy. The watched shipping-GPU control used a green colour texture and
a deliberately magenta two-texel mask: source alpha 31 was rejected, while source alpha 32 produced
green without leaking mask RGB, and no kernel GPU fault was observed. A guarded 120-present recomp
audit advanced all 52 perspective observations through classification, both resource decodes,
scene readiness, and native submission, reaching 572 lit models among 15,860 total with zero layout,
projection, rigid-matrix, or mesh-decode failures. The independent guarded 400-present native-decomp
run remained healthy at 42,033 total/666 lit models but encountered zero instances of this exact
dynamic material, so it is not claimed as decomp reached-scene evidence.

## Re-confirmed 2026-08-31 — vertex-diffuse plus triple specular

Exact CPU controls decode the second two-channel program as texture times vertex-colour diffuse plus three times directional specular and reject a changed stage or missing vertex colour. The watched shipping affine GPU control remained clean. A guarded 120-present recomp audit advanced both reached texture variants through 100/100 classification, decode, perspective readiness, and native submission, raising total models from 8,704 to 8,804 and exiting 0 under the live GPU watcher.
