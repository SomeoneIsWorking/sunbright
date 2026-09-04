---
id: C046
kind: claim
status: holds
created: 2026-08-13
tags: interpolation,geometry,re
---

## Claim

GMSE01 far-LOD grass uses direct signed-16 XYZ positions whose VAT fractional shift must be
preserved when reconstructing interpolated vertices.

## Evidence

An instrumented Dolphin FIFO capture established the vertex format. A guarded stage-8 observation
then paired 391 of 392 deforming grass draws across 11,368 `drawFar` calls.

## What would falsify it

An independent stage-8 capture decodes a different position format or VAT shift, or reaches
`drawFar` while the identified grass population does not use that layout.
