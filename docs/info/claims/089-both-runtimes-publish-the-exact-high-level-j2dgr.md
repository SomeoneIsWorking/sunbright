---
id: C089
kind: claim
status: holds
created: 2026-08-30
tags: semantic-renderer,j2d,rectangle
depends: native-render/src/solid_rectangle.cpp#resolve_transformed_s16_rectangle, sms-recomp/overrides/semantic_j2d_fill_box.cpp#override_j2d_fill_box, decomp/sms/src/JSystem/J2D/J2DGrafContext.cpp#J2DGrafContext::fillBox
---

## Claim

Both runtimes publish the exact high-level J2DGrafContext filled-box values to the shared PC-native semantic renderer while retaining the original game body.

## Evidence

GMSE01 0x802eba70 generated body and decomp source; recomp big-endian and decomp production-linked controls for signed-16-bit narrowing, matrix transform, four corner colours, and target-pixel scissor; 43/43 root and 30/30 recomp tests; watched semantic GPU control; guarded 180-present Delfino safety run (the scene did not call fillBox).

## What would falsify it

Falsified if a controlled retail trace disagrees on signed-16-bit narrowing, position transform, bottom-corner colour ownership, or scissor space; if either runtime reaches J2DGrafContext::fillBox with semantic collection active without one ordered J2dGrafContextFillBox submission; or if either adapter skips its original body.
