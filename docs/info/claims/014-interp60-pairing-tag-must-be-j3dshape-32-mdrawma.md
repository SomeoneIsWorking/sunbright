---
id: C014
kind: claim
status: holds
created: 2026-08-04
tags: interpolation,j3d,re
depends: decomp/sms/src/JSystem/J3D/J3DGraphBase/J3DShape.cpp#J3DShapePacket::draw
---

## Claim

An interpolation identity for a J3D draw must include both `J3DShape` and the active draw-matrix
owner. A shape alone identifies shared model data and collapses multiple instances into unstable
draw-order pairing.

## Evidence

`J3DShapePacket::draw` installs the instance's matrices into the shared shape for each draw. A
controlled 598-tick comparison reduced mean object motion from 48.3 to 14.9 when the matrix owner was
added to the identity.

## What would falsify it

A supported J3D path proves that each shape has exactly one instance, or draws a shape without
installing an instance-specific matrix owner.
