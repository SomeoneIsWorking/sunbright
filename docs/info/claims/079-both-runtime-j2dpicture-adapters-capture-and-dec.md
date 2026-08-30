---
id: C079
kind: claim
status: holds
created: 2026-08-28
tags: renderer,resources,recomp,decomp
depends: sms-recomp/overrides/j2d_picture_adapter.cpp#capture_j2d_picture, sms-boot/runtime/native_picture_adapter.cpp#sb_native_picture_submit, native-render/src/semantic_sink.cpp#submit_picture
reconfirmed: 2026-08-30
verified_at: 2026-08-30 06:16:40
---

## Claim

Both runtime J2DPicture adapters capture and decode exact JUT texel/palette content at draw entry and atomically submit each semantic command with matching versioned RGBA images before the retained GX body can mutate or reuse the source storage.

## Evidence

Recomp j2d_picture_adapter controls cover RGBA8, C4+IA8, changed revisions, and short-palette refusal. Production-linked decomp native_picture_adapter controls use real J2DPicture/JUTTexture/JUTPalette objects and verify C4+IA8/I8 bytes, stable/changed revisions, synchronous span copying, absent-sink no-allocation, and host-allocation depth one. Root 38/38 and recomp 28/28 CTests pass; the semantic GPU picture control passed under gpu_watch with no kernel fault.

## What would falsify it

Either adapter reads texture bytes after its retained GX body, a sink can accept a command without matching image content, a source/palette mutation is missed by the revision, or the decomp callback occurs outside the host-allocation gate.

## Re-confirmed 2026-08-28

Reconfirmed after contextual PictureDraw submission: production-linked decomp adapter test and recomp guest adapter test pass exact decoded pixels, stable/changed revisions, transient copying, host-allocation depth, and canvas/clip attachment.

## Re-confirmed 2026-08-30

Reconfirmed after exclusive sink leases and frame-bridge integration: recomp j2d_picture_adapter and production-linked decomp native_picture_adapter controls pass exact pixels, revisions, refusal paths, transient copying, and host-allocation depth; root 42/42 and recomp 27/27 tests pass, and the watched GPU control remains fault-free.

## Re-confirmed 2026-08-30

Reverified decoded-image ownership after shared material capture was reused by the immediate path: recomp guest and production-linked decomp controls pass exact pixels, revisions, synchronous image copying, refusal paths, and host-allocation balance; full suites and watched GPU control pass.

## Re-confirmed 2026-08-30

Reverified after extracting BigEndianGuestReader and the shared native J2D bridge: both picture adapter controls still distinguish valid/short/changed image content, the full Clang suites pass, and the guarded Delfino run completed without image capture refusal.

## Re-confirmed 2026-08-30

Reconfirmed after extending the shared J2D context with target-pixel scissor state: picture decode/lifetime controls remain green in 43/43 root/decomp and 30/30 recomp tests.
