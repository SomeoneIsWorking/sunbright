# 2026-08-31 — vertex-lit RGB with material opacity

After the directional colour ramp landed, the largest stage-two perspective fallback was 72
instances of a one-texture, one-stage material. Its colour equation was already supported: decoded
texture multiplied by diffuse-lit mesh vertex RGB. The classifier rejected it because it only
recognized the variant whose opacity also comes from mesh vertex alpha. This reached variant uses
the authored material alpha instead.

The existing `LitTexturedMaterial` already carries RGB and alpha source choices independently, so a
new material or shader would have duplicated the same equation. The shared J3D classifier now
admits the vertex-RGB/material-alpha combination while retaining the exact texture binding, complete
colour program, normal, lighting context, and raster-policy checks. Both recomp and native-decomp
layout adapters already use that classifier.

The CPU control assigns non-default material alpha, verifies that it is published while vertex
alpha is disabled, and changes the reached colour program to prove the classifier rejects a
different equation. The focused classifier test passed. A guarded 20-present stage-two recomp audit then
advanced all 72 instances through classification, texture decode, scene readiness, and native model
submission. Coverage rose from 1,656 models/1,407,168 vertices to 1,728 models/1,422,936 vertices,
including 1,392 lit models. The game exited normally and the live kernel watcher reported no GPU
fault or reset. The pre-existing Vulkan shader-module teardown warning remains a separate cleanup
defect rather than a GPU crash.
