---
id: 27
title: Semantic renderer omits resource-font glyphs
status: resolved
symptom: PC-native semantic frames contain pictures and rectangles but no JUTResFont glyphs, so HUD labels and numbers disappear if the semantic frame is presented
state_items: S004,S005
tags: renderer,semantic,j2d,text
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

The semantic schema had no font-glyph operation, and neither runtime published the high-level state
needed to create one. A glyph cannot be reconstructed safely from the GX stream without returning
to GameCube rendering semantics: its intended rectangle depends on `JUTResFont` width/bearing
state and its final position and clip belong to the surrounding `J2DTextBox` transform.

## What was tried / dead ends

Treating glyphs as ordinary `J2DPicture` calls would erase the semantic distinction and still omit
the font-specific advance, bearing, selected page, black/white remap, and text-box transform. The
implementation instead shares only the renderer's decoded-image material machinery.

## Resolution

Added a renderer-neutral glyph command and one retail-derived layout resolver used by both
runtimes. Recomp overrides retain the original text-box, font-state, and character bodies while
copying selected big-endian font data afterward. Decomp source publishes the corresponding native
values at the same boundaries. CPU controls cover valid and malformed font data, layout and atlas
rounding; a production-linked decomp control uses real encoded page bytes; the watched SDL GPU
control renders glyphs in mixed-family order. A guarded Delfino run observed 1,040 real glyph
submissions among 1,209 ordered operations. Aurora remains the visible renderer because the wider
semantic path still lacks windows, generic fills, 3D, particles, and effects.

### Resolution (2026-08-30)
Added one renderer-neutral resource-font glyph operation, shared retail layout resolver, retained-body recomp and decomp adapters, malformed-input controls, production-linked native adapter coverage, guarded mixed-family GPU coverage, and a real Delfino run with 1,040 glyph submissions.
