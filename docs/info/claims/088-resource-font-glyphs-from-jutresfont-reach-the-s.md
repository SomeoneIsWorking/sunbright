---
id: C088
kind: claim
status: holds
created: 2026-08-30
tags: semantic-renderer,j2d,text
depends: native-render/src/glyph.cpp#resolve_resource_glyph_layout, sms-recomp/overrides/semantic_resource_font_glyph.cpp#run_font_glyph, decomp/sms/src/JSystem/JUtility/JUTResFont.cpp#JUTResFont::drawChar_scale
---

## Claim

Resource-font glyphs from JUTResFont reach the shared PC-native semantic renderer in authored order in the recomp runtime, and both runtimes implement the same renderer-neutral glyph layout and decoded-page contract while retaining their original game bodies.

## Evidence

GMSE01 bodies 0x802d0b28, 0x802d0dd8, 0x802f178c, 0x802f1864, and 0x802f1b00; shared glyph/layout, recomp big-endian adapter, production-linked decomp adapter, mixed-family GPU controls; guarded 180-present Delfino run reported 1,040 glyphs among 1,209 operations.

## What would falsify it

Falsified if a controlled retail trace disagrees on bearing, advance, atlas coordinates, transform, clip, remap, or corner ownership; if a semantic-enabled recomp run reaches JUTResFont glyph drawing without an ordered glyph submission; or if either adapter skips its original game body.
