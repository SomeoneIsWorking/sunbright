---
id: C092
kind: claim
status: holds
created: 2026-08-30
tags: renderer,j3d,semantic
depends: native-render/src/model.cpp#transform_vertex, decomp/sms/src/JSystem/J3D/J3DGraphBase/J3DShape.cpp#J3DShape::draw
---

## Claim

The renderer-neutral J3D sink accepts nonzero rigid, unlit, single-texture model geometry while
keeping mesh decoding, pose binding, image selection, and projection failures explicit.

## Evidence

Focused model controls submit decoded vertices through the accepted material family and separately
exercise unreadable layout, missing projection, invalid matrix binding, mesh-decode, and texture-
table refusals.

## What would falsify it

The accepted family submits no vertices, an invalid layout is accepted, a valid rigid binding moves
the wrong vertex, or a failure path becomes silent.
