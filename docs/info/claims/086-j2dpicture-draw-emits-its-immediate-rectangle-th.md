---
id: C086
kind: claim
status: holds
created: 2026-08-30
tags: semantic-renderer,j2d,re
depends: decomp/sms/src/JSystem/J2D/J2DPicture.cpp#J2DPicture::draw, native-render/src/picture.cpp#resolve_direct_picture_layout
---

## Claim

`J2DPicture::draw` emits its immediate rectangle through the `makeMatrix` position transform,
narrows width and height to signed 16-bit vertex coordinates, and uses its three booleans for
horizontal reversal, vertical reversal, and transposed UV ownership.

## Evidence

The GMSE01 body at `0x802ccef4` matches recovered `J2DPicture.cpp`. Resolver controls cover every
boolean combination, signed narrowing, transform ownership, and UV association.

## What would falsify it

A DOL listing or controlled trace at `0x802ccef4` shows different positions, narrowing, UV
association, or matrix ownership.
