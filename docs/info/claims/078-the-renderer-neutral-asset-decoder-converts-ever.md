---
id: C078
kind: claim
status: holds
created: 2026-08-28
tags: renderer,assets,j2d
depends: native-render/src/image_decode.cpp#decode_image_rgba8, sms-recomp/runtime/render/gx_texture.cpp#gx_decode_texture
reconfirmed: 2026-08-28
verified_at: 2026-08-28 03:32:53
---

## Claim

The renderer-neutral asset decoder converts every retail tiled image encoding and all palette encodings to RGBA8 with validated ranges and exact GameCube component/CMPR rules, and the recomp GX compatibility adapter consumes that same implementation without moving guest or GX ownership into native-render.

## Evidence

native_render_image_decode exercises all 11 image formats, all 3 palette formats, tile/mip sizing, bit replication, both CMPR branches, range failures, palette-index refusal, and revision changes; full root 37/37 and recomp 28/28 CTest suites pass under Clang.

## What would falsify it

A known game texture differs from the decoded RGBA bytes, an accepted source/palette range can be read out of bounds, or the GX compatibility adapter is shown to use a separate decoder.

## Re-confirmed 2026-08-28

native_render_image_decode still exercises all 11 image formats, all 3 palette formats, tile/mip sizing, bit replication, both CMPR branches, range failures, palette-index refusal, and revision changes; revisions are now explicitly nonzero for runtime resources. Root 38/38 and recomp 28/28 CTest suites pass under Clang.
