---
id: C095
kind: claim
status: holds
created: 2026-08-31
tags: renderer,j3d,lighting,recomp,decomp
depends: native-render/src/j3d_lit_material.cpp#classify_j3d_lit_textured_material, native-render/src/j3d_specular_material.cpp#classify_j3d_specular_textured_material, native-render/src/model.cpp#transform_vertex, sms-recomp/overrides/semantic_j3d_lighting.cpp#publish_lighting, sms-boot/runtime/native_j3d_lighting.cpp#sb_native_j3d_publish_stage_lighting, decomp/sms/src/MarioUtil/LightUtil.cpp#TLightCommon::setLight
reconfirmed: 2026-08-31
verified_at: 2026-08-31 07:45:14+00:00
---

## Claim

Both runtimes feed high-level stage ambient, point lights, directional-specular direction and shininess, decoded normals, and exact single-texture diffuse/specular material values into the shared PC-native J3D renderer without consuming GX light state.

## Evidence

Focused CPU controls passed; a guarded 60-present recomp Delfino audit published 400/400 high-level light updates and submitted 60 lit models among 2,900 total; a guarded 400-present native-decomp Delfino audit submitted 36 lit models among 36,948 total; both runs exited 0 under the live GPU watcher.

## What would falsify it

Either adapter reads GX/XF/FIFO light state, the source-native and recomp high-level light inputs no longer classify the same exact material families, the focused lighting controls fail, a reached guarded Delfino run submits zero lit models, an altered specular channel/program is accepted, or an original J3D/setLight body stops executing.

## Re-confirmed 2026-08-31

Focused CPU material/model controls passed; a guarded 60-present recomp Delfino audit published 400/400 high-level light updates and submitted 100 lit models among 3,260 total; a guarded 400-present native-decomp Delfino audit submitted 630 lit models among 41,997 total; both exited 0 under the live GPU watcher. The pass-all alpha-policy control accepts the equivalent AND/OR/XNOR encodings and rejects XOR.

## Re-confirmed 2026-08-31 — directional specular

Exact channel, tint-selector, stage, normal, and shininess controls passed. The watched shipping-shader control distinguished texture-times-diffuse from the affine red-tint result while preserving green through the exact sRGB sample/output conversion, with no kernel GPU fault. A guarded 60-present recomp Delfino audit published 400/400 high-level light updates with zero shininess failures and submitted all 60 perspective-reached `_mat_hand3_L` specular models, raising the lit count to 160 among 3,320 total. A guarded 400-present native-decomp audit raised the lit count to 666 among 42,033 total. Both exited 0 under the live GPU watcher.
