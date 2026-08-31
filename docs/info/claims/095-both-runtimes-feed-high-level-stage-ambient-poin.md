---
id: C095
kind: claim
status: holds
created: 2026-08-31
tags: renderer,j3d,lighting,recomp,decomp
depends: native-render/src/j3d_lit_material.cpp#classify_j3d_lit_textured_material, native-render/src/model.cpp#transform_vertex, sms-recomp/overrides/semantic_j3d_lighting.cpp#publish_lighting, sms-boot/runtime/native_j3d_lighting.cpp#sb_native_j3d_publish_stage_lighting, decomp/sms/src/MarioUtil/LightUtil.cpp#TLightCommon::setLight
---

## Claim

Both runtimes feed high-level stage ambient, point lights, decoded normals, and exact single-texture diffuse material values into the shared PC-native J3D renderer without consuming GX light state.

## Evidence

Focused CPU controls passed; a guarded 60-present recomp Delfino audit published 400/400 high-level light updates and submitted 60 lit models among 2,900 total; a guarded 400-present native-decomp Delfino audit submitted 36 lit models among 36,948 total; both runs exited 0 under the live GPU watcher.

## What would falsify it

Either adapter reads GX/XF/FIFO light state, the source-native and recomp high-level light inputs no longer classify the same exact material family, the focused lighting controls fail, a reached guarded Delfino run submits zero lit models, or an original J3D/setLight body stops executing.
