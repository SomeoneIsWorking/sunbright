---
id: C081
kind: claim
status: holds
created: 2026-08-30
tags: renderer,architecture
depends: native-render/src/semantic_frame_bridge.cpp#SemanticFrameBridge::begin, native-render/src/semantic_frame_bridge.cpp#SemanticFrameBridge::seal, sms-recomp/overrides/native_frame.cpp#present_tail, sms-boot/runtime/frame_seam.cpp#sb_frame_present
---

## Claim

Both runtime frame seams call one lease-owned semantic frame bridge at the exact simulation-frame boundaries, and the bridge remains inert until host composition explicitly activates it.

## Evidence

Clang sms-boot and sms-recomp builds pass; native_render_semantic_frame_bridge and both runtime adapter controls pass. The bridge control proves inactive begin/seal, exclusive sink lease, wrong-owner refusal, exact seal storage, and teardown while collecting. Recomp seals in present_tail immediately before gxfifo_build after retained wait work and begins after optional subframes; decomp begins at sb_frame_seam_start and seals inside the sb_frame_present host-allocation gate.

## What would falsify it

A runtime publishes a semantic draw after its seal but before the corresponding GX frame closes, an unrelated sink owner can steal or clear the lease, or semantic collection becomes active without an explicit host activation.
