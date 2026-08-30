---
id: C086
kind: claim
status: holds
created: 2026-08-30
tags: semantic-renderer,j2d
depends: decomp/sms/src/JSystem/J2D/J2DPicture.cpp#J2DPicture::draw, native-render/src/picture.cpp#resolve_direct_picture_layout
reconfirmed: 2026-08-30
verified_at: 2026-08-30 05:45:16
---

## Claim

J2DPicture::draw emits its immediate rectangle through the makeMatrix-built position transform, narrows width and height to signed 16-bit vertex coordinates, and uses the three boolean arguments for horizontal reversal, vertical reversal, and transposed UV ownership.

## Evidence

GMSE01 generated body at 0x802ccef4, matching decomp J2DPicture.cpp source, close resolver/adapter controls, and guarded 400-present Delfino semantic audit on 2026-08-30.

## What would falsify it

A DOL listing or controlled capture at 0x802ccef4 shows different emitted positions, narrowing, UV association, or matrix ownership for any boolean combination.

## Re-confirmed 2026-08-30

Reverified after moving shared matrix operations out of picture.cpp: direct-picture resolver and both runtime adapter controls pass, the full Clang suites pass, and the guarded Delfino run completed without direct-picture context or capture refusal.
