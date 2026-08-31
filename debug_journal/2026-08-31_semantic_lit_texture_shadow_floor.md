# 2026-08-31 — lit texture with an authored shadow floor

The next stage-two fallback contained 72 perspective-reached textured models. Its one-stage colour
program differs from the ordinary texture-times-diffuse family in one input: when diffuse-lit mesh
vertex RGB is zero, the output uses the selected 1/8 constant instead of black. Decoding the
complete operation gives the ordinary per-channel equation:

`texture * light + 1/8-grey * (1 - light)`

This is a property of the existing PC-native lit-texture material, not a new renderer or a console
combiner abstraction. `LitTexturedMaterial` now carries an optional shadow colour. Its vertex
transform publishes the lit value as the texture multiplier and `shadow * (1 - light)` as the
additive term consumed by the existing texture shader. The exact source program and consumed 1/8
selector remain classifier admission gates and do not cross into the renderer interface. Both
layout-local runtime adapters already consume this shared classifier.

The classifier control accepts the reached program and 1/8 selector, then changes only that
selector and requires rejection. The model control uses a known ambient light and mesh vertex colour
to verify both the multiplicative lit term and the complementary grey contribution, including
independent vertex alpha. The focused classifier and model tests passed.

A guarded 20-present recomp audit advanced all 72 instances through classification, texture decode,
scene readiness, and native submission. Coverage rose from 1,728 models/1,422,936 vertices to
1,800 models/1,559,592 vertices, including 1,464 lit models. The game exited normally and the live
kernel watcher reported no GPU fault or reset. The pre-existing Vulkan shader-module teardown
warning remains a separate cleanup defect rather than a GPU crash.
