---
id: C078
kind: claim
status: holds
created: 2026-08-28
tags: renderer,assets,j2d
depends: native-render/src/image_decode.cpp#decode_image_rgba8
---

## Claim

The renderer-neutral asset decoder converts all 11 retail tiled image encodings and all three
palette encodings to RGBA8 with validated ranges and exact GameCube component and CMPR rules.

## Evidence

Focused controls cover every format, tile and mip sizing, bit replication, both CMPR branches,
range failures, palette-index refusal, stable content revisions, changed content revisions, and the
requirement that runtime-resource revisions are nonzero.

## What would falsify it

A known retail texture differs from the decoded bytes, an accepted image or palette span permits an
out-of-range read, or a content change fails to change the revision.
