# Semantic resource-font glyphs in the PC-native renderer

## Root cause

The renderer-neutral stream carried pictures and solid rectangles but had no operation for
`JUTResFont` glyphs. The missing data was not just a texture quad: the character body selects a
glyph page and width entry, applies proportional or fixed advance and optional left bearing, derives
15-bit fixed-point atlas coordinates, and inherits its final transform and clip from the surrounding
`J2DTextBox`. Recovering only the GX quad would have moved the native renderer back below GX.

## Grounded contract

The GMSE01 bodies at `JUTResFont::drawChar_scale` `0x802f1b00`, `JUTResFont::setGX` `0x802f178c`
and `0x802f1864`, `J2DTextBox::draw` `0x802d0b28`, and the matrix-taking
`J2DTextBox::drawSelf` `0x802d0dd8` establish the boundary. The shared resolver owns the exact
bearing/advance and UV arithmetic. Runtime adapters own layout access only: recomp reads guest
big-endian fields, while decomp publishes typed native values. Neither shares game objects or
layouts with the other.

Each override or source hook retains the original game body. Glyphs remain a distinct
`SemanticDraw` alternative for coverage and ordering, while the SDL pass deliberately reuses the
same decoded RGBA image material and shader path as pictures.

## Controls and observed coverage

- Shared CPU controls distinguish proportional/fixed width, bearing on/off, transformed corners,
  retail fixed-point UV rounding, invalid extents, and glyph versus picture family identity.
- The recomp adapter control drives valid and malformed big-endian INF1/WID1/GLY1-like state,
  checks page selection, corner order, atlas decoding, content revisions, and short reads.
- The production-linked decomp control sends actual encoded font-page bytes through the native
  adapter and proves that the sink copies transient decoded pixels before return.
- The watched SDL GPU control first validates the watcher with planted faults, then renders glyphs
  through the mixed semantic pass without a kernel GPU fault.
- A guarded 180-present Delfino run completed 90 semantic frames and reported 1,209 operations:
  160 pictures, 1,040 resource-font glyphs, and nine solid rectangles.

The bounded decomp title and stage-one runs reported zero resource-font glyphs on the particular
paths they reached. They prove that the integrated hooks do not break those runs, but they do not
prove live decomp glyph coverage. That evidence remains the production-linked native adapter
control until a decomp route actually exercises the font family.

## Remaining boundary

The semantic output remains offscreen. J2D windows, `J2DGrafContext::fillBox`, 3D/J3D, particles,
lights, and effects are still absent, so presenting or overlaying this partial output would change
the authored frame.
