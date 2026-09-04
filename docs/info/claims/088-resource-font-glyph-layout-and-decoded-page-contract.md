---
id: C088
kind: claim
status: holds
created: 2026-08-30
tags: semantic-renderer,j2d,text,re
depends: native-render/src/glyph.cpp#resolve_resource_glyph_layout, decomp/sms/src/JSystem/JUtility/JUTResFont.cpp#JUTResFont::drawChar_scale
---

## Claim

The renderer-neutral JUT resource-font contract preserves authored glyph order, bearing, advance,
atlas coordinates, transform, clip, colour remap, corner ownership, and decoded page content.

## Evidence

GMSE01 bodies at `0x802d0b28`, `0x802d0dd8`, `0x802f178c`, `0x802f1864`, and `0x802f1b00` agree with
the recovered font source. Focused layout, decoded-page, mixed-family ordering, and GPU controls
exercise those values.

## What would falsify it

A controlled retail trace disagrees on any layout or colour field, or the semantic pass changes the
authored glyph order or decoded page content.
