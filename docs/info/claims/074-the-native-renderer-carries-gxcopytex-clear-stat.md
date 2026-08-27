---
id: C074
kind: claim
status: holds
created: 2026-08-28
tags: render,recomp,native,efb-copy,clear
depends: sms-recomp/runtime/render/native_efb_copy_plan.cpp, sms-recomp/runtime/render/native_efb_copy_clear_draw.cpp, sms-recomp/tests/native_efb_copy_plan_test.cpp
---

## Claim

The recomp native renderer carries `GXCopyTex(clear=true)` colour, alpha, depth, update masks, and
clipped source rectangle through its typed copy plan and expresses the post-copy clear as the first
ordered batch after the copy barrier.

## Evidence

`native_efb_copy_plan_test` exercises asymmetric AR/GB bytes, 24-bit depth normalization,
independent write masks, partial clipping, an offscreen no-op, colour-only depth disable, and the
copy epoch before/after the synthetic clear. The combined Clang build and focused test passed on
2026-08-28. Aurora and Dolphin independently establish the copy-then-rectangular-clear contract.

## What would falsify it

Any change to BP `0x40`, `0x41`, `0x4F`-`0x52` parsing, `NativeEfbCopyRequest`, scene copy ordering,
the copy plan/clear-draw builder, batch merging, scissor application, or colour/depth write-mask
translation requires the focused test and a live native/Aurora parity control to be rerun.
