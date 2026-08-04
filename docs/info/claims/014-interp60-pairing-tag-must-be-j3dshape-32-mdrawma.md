---
id: C014
kind: claim
status: holds
created: 2026-08-04
tags: interp60,j3d
---

## Claim

interp60 pairing tag must be (J3DShape << 32 | mDrawMatrices), not J3DShape alone: a J3DShape belongs to the shared J3DModelData, and J3DShapePacket::draw swaps the instance's matrices into it per draw, so shape-only tags collapse every instance of a model into one identity and pair by unstable draw order.

## Evidence

debug_journal/2026-08-04_interp60_pairing_attribution.md; object motion mean 48.3 -> 14.9 at equal N=598 ticks

## What would falsify it

if a future J3D path draws a shape without the packet installing per-instance mDrawMatrices, the instance half of the tag becomes constant and pairing silently degrades to the old behaviour
