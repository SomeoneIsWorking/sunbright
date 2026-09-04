---
id: C089
kind: claim
status: holds
created: 2026-08-30
tags: semantic-renderer,j2d,rectangle,re
depends: native-render/src/solid_rectangle.cpp#resolve_transformed_s16_rectangle, decomp/sms/src/JSystem/J2D/J2DGrafContext.cpp#J2DGrafContext::fillBox
---

## Claim

`J2DGrafContext::fillBox` maps signed-16-bit bounds, its position transform, four corner colours,
and target-pixel scissor directly to one renderer-neutral filled-box operation.

## Evidence

The GMSE01 body at `0x802eba70` agrees with recovered source. Focused controls distinguish signed
narrowing, matrix transformation, bottom-corner colour ownership, and scissor coordinate space;
the mixed-family GPU control consumes the same operation.

## What would falsify it

A retail listing or controlled trace disagrees on narrowing, transform, corner-colour ownership, or
scissor space, or collection changes the operation before rendering.
