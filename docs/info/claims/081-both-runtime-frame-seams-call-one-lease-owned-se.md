---
id: C081
kind: claim
status: holds
created: 2026-08-30
tags: renderer,architecture
depends: native-render/src/semantic_frame_bridge.cpp#SemanticFrameBridge::begin, native-render/src/semantic_frame_bridge.cpp#SemanticFrameBridge::seal, native-render/src/sdl_semantic_frame_client.cpp#SdlSemanticFrameClient::encode_last_sealed, sms-recomp/host/render_composition.cpp#RenderComposition::encode_semantic_frame, sms-recomp/overrides/native_frame.cpp#present_tail, sms-boot/runtime/semantic_render.cpp#sb_semantic_render_consume, sms-boot/runtime/frame_seam.cpp#sb_frame_present
reconfirmed: 2026-08-30
verified_at: 2026-08-30 04:46:12
---

## Claim

Both runtime frame seams call one lease-owned semantic frame bridge at the exact simulation-frame boundaries, and the bridge remains inert until host composition explicitly activates it.

## Evidence

Clang sms-boot and sms-recomp builds pass; native_render_semantic_frame_bridge and both runtime adapter controls pass. The bridge control proves inactive begin/seal, exclusive sink lease, wrong-owner refusal, exact seal storage, and teardown while collecting. Recomp seals in present_tail immediately before gxfifo_build after retained wait work and begins after optional subframes; decomp begins at sb_frame_seam_start and seals inside the sb_frame_present host-allocation gate.

## What would falsify it

A runtime publishes a semantic draw after its seal but before the corresponding GX frame closes, an unrelated sink owner can steal or clear the lease, or semantic collection becomes active without an explicit host activation.

## Re-confirmed 2026-08-30

Reverified the exact shared-library bridge and host frame lifecycle after the mixed-stream change.
The root 42/42 and recomp 28/28 tests pass. The bridge control proves sealed sequence increments
and duplicate-consumption refusal. Guarded title runs activated the bridge in both hosts and
completed every submitted semantic frame: 50/50 for recomp and 400/400 for decomp, with each encode
after seal and before the next begin.

## Re-confirmed 2026-08-30

Reverified unchanged frame-bridge boundaries after direct-picture integration: root 42/42 and recomp 28/28 tests pass; guarded 400-present recomp and decomp audits each completed every submitted frame and exited cleanly.
