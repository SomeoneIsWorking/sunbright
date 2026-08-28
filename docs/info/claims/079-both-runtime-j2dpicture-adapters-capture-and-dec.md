---
id: C079
kind: claim
status: holds
created: 2026-08-28
tags: renderer,resources,recomp,decomp
depends: sms-recomp/overrides/j2d_picture_adapter.cpp#capture_j2d_picture, sms-boot/runtime/native_picture_adapter.cpp#sb_native_picture_submit, native-render/src/picture_sink.cpp#submit_picture
---

## Claim

Both runtime J2DPicture adapters capture and decode exact JUT texel/palette content at draw entry and atomically submit each semantic command with matching versioned RGBA images before the retained GX body can mutate or reuse the source storage.

## Evidence

Recomp j2d_picture_adapter controls cover RGBA8, C4+IA8, changed revisions, and short-palette refusal. Production-linked decomp native_picture_adapter controls use real J2DPicture/JUTTexture/JUTPalette objects and verify C4+IA8/I8 bytes, stable/changed revisions, synchronous span copying, absent-sink no-allocation, and host-allocation depth one. Root 38/38 and recomp 28/28 CTests pass; the semantic GPU picture control passed under gpu_watch with no kernel fault.

## What would falsify it

Either adapter reads texture bytes after its retained GX body, a sink can accept a command without matching image content, a source/palette mutation is missed by the revision, or the decomp callback occurs outside the host-allocation gate.
