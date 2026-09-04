---
id: C087
kind: claim
status: holds
created: 2026-08-30
tags: renderer,j2d,re
depends: decomp/sms/src/JSystem/J2D/J2DGrafContext.cpp#J2DGrafContext::J2DGrafContext
---

## Claim

Retail `J2DGrafContext` does not initialize its type word, while `J2DOrthoGraph` installs vtable
`0x803e14b0` and type 1. An active-canvas classifier must require both values, and native-layout
construction must initialize its discriminator explicitly.

## Evidence

GMSE01 bodies at `0x802eb460` and `0x802eb51c` omit the `+0x04` write; constructors at `0x802ecfcc`
and `0x802ed0a8` install the orthographic vtable and write 1. A stale-type/base-vtable negative
control refuses the false orthographic classification.

## What would falsify it

Retail code writes a stable base discriminator, another supported orthographic context uses a
different identity, or a valid orthographic setup is refused by the combined classifier.
