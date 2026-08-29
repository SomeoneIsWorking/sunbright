---
id: C077
kind: claim
status: holds
created: 2026-08-28
tags: renderer,semantic
depends: native-render/src/picture_pass.cpp#PicturePass::encode, sms-recomp/overrides/j2d_picture_adapter.cpp#capture_j2d_picture, sms-boot/runtime/native_picture_adapter.cpp#sb_native_picture_submit
reconfirmed: 2026-08-30
verified_at: 2026-08-30 02:07:19
---

## Claim

Both runtime adapters publish a shared above-GX J2DPicture command before retaining their original draw bodies, and the independent SDL3 semantic picture pass renders decoded RGBA controls without consuming FIFO, TEV, or Aurora state.

## Evidence

Clang builds passed for sms-boot and sms-recomp; 36/36 root tests and 28/28 recomp tests passed; watched native_render_picture_gpu_test verified clipped 2x2 quadrants, half alpha, exact repeat, no-op fully clipped draw, and changed revision/content with no kernel GPU fault.

## What would falsify it

Any adapter is observed deriving the command from post-body GX/FIFO state, either original body is not called, the semantic GPU controls return the wrong equality/difference, or the watcher records a GPU fault.

## Re-confirmed 2026-08-28

Clang builds passed for sms-boot and sms-recomp; 38/38 root tests and 28/28 recomp tests passed; watched native_render_picture_gpu_test again verified clipped 2x2 quadrants, half alpha, exact repeat, no-op fully clipped draw, and changed revision/content with no kernel GPU fault after the decoded-image submission contract change.

## Re-confirmed 2026-08-28

Reconfirmed after per-draw canvas/viewport schema: root 40/40 tests, recomp 28/28 tests, and guarded full-canvas/sub-viewport picture GPU controls pass without a kernel GPU fault.

## Re-confirmed 2026-08-30

Reconfirmed after the shared-platform and caller-owned encode refactor: Clang sms-boot/sms-recomp builds pass, root 42/42 and recomp 27/27 tests pass, both J2DPicture adapter controls pass, and the watched semantic GPU test proves the sRGB picture pass, changed revision, and current-revision residency without a kernel GPU fault.
