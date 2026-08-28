---
id: C077
kind: claim
status: holds
created: 2026-08-28
tags: renderer,semantic
depends: native-render/src/picture_pass.cpp#PicturePass::render_and_readback, sms-recomp/overrides/j2d_picture_adapter.cpp#capture_j2d_picture, sms-boot/runtime/native_picture_adapter.cpp#sb_native_picture_submit
reconfirmed: 2026-08-28
verified_at: 2026-08-28 03:32:46
---

## Claim

Both runtime adapters publish a shared above-GX J2DPicture command before retaining their original draw bodies, and the independent SDL3 semantic picture pass renders decoded RGBA controls without consuming FIFO, TEV, or Aurora state.

## Evidence

Clang builds passed for sms-boot and sms-recomp; 36/36 root tests and 28/28 recomp tests passed; watched native_render_picture_gpu_test verified clipped 2x2 quadrants, half alpha, exact repeat, no-op fully clipped draw, and changed revision/content with no kernel GPU fault.

## What would falsify it

Any adapter is observed deriving the command from post-body GX/FIFO state, either original body is not called, the semantic GPU controls return the wrong equality/difference, or the watcher records a GPU fault.

## Re-confirmed 2026-08-28

Clang builds passed for sms-boot and sms-recomp; 38/38 root tests and 28/28 recomp tests passed; watched native_render_picture_gpu_test again verified clipped 2x2 quadrants, half alpha, exact repeat, no-op fully clipped draw, and changed revision/content with no kernel GPU fault after the decoded-image submission contract change.
