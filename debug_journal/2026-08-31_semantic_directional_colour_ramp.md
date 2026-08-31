# 2026-08-31 — semantic directional colour ramp

The stage-two semantic audit exposed 300 observations of one untextured, opaque, two-stage J3D
material. Of those, 192 occurred in perspective scenes. Before this change all 192 visible
instances fell back to the retained GameCube renderer.

The first authored stage uses the secondary directional-specular colour as a coordinate. Decoding
its complete colour program gives `min(4, 2 + 8h)` for highlight component `h`; the signed upper
bound is confirmed by Dolphin's pixel-shader implementations in `UberShaderPixel.cpp` and
`PixelShaderGen.cpp`. The second stage uses that coordinate to interpolate from colour register 1
to colour register 0, then adds `h`. The PC-native material therefore evaluates the high-level
per-channel equation directly:

`h + lerp(lower_colour, upper_colour, min(4, 2 + 8h))`

The renderer contract carries only the two authored colours, high-level directional light,
material alpha, and ordinary opaque raster policy. The exact console channels and stage programs
remain at the adapter admission boundary and do not enter the renderer. Both the recomp and native
decomp layout adapters compile through the same classifier; neither shares game objects or layout.

The production-path CPU control accepts the exact reached material, verifies the observed colour
endpoints, and exercises the curve with both aligned and back-facing normals. Independent negative
controls change the second stage, change the raster source, and remove the authored register
colours. A directional-only control sets the point-light count to zero and still succeeds, proving
that the classifier does not require an unused light source.

The focused classifier executable passed, and both Clang builds (`sms-recomp` and `sms-boot`)
completed. A guarded 20-present recomp audit then advanced the family through
300/300 classification and 192/192 visible model submissions. Total native coverage rose from
1,464 models/1,211,328 vertices to 1,656 models/1,407,168 vertices, including 1,320 lit models. The
game exited normally and the live kernel watcher recorded no GPU fault or reset. Dawn still reports
one live Vulkan shader module during device teardown; that is a separate cleanup defect, not a GPU
crash, and this change does not claim to resolve it.
