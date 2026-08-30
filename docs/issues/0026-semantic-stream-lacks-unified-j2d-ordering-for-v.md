---
id: 26
title: Semantic stream lacks unified J2D ordering for visible presentation
status: resolved
symptom: The offscreen semantic target preserves picture/GC2D-fill order, but presenting it would still lose authored ordering against text, windows, generic J2D fills, direct picture calls, and later 3D/effect families.
state_items: S004,S005
tags: renderer,semantic,j2d,ordering,presentation
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

The game's actual J2D stream interleaves pictures with text glyphs, window pieces, multiple solid
fill entry points, and immediate picture calls which can occur outside `J2DScreen::draw`. The first
vertical slice defined only `PictureCommand`; subsequent slices added the title-visible `GC2D
fill_rect` family and the active-context immediate-picture path, but the remaining families still
prevent replacement or overlay without changing authored draw order.

## What was tried / dead ends

Presenting the current offscreen target remains ruled out: non-clear pixels and mixed-family frames
prove liveness and local order, not complete frame ownership. An overlay would hide or reorder the
semantic families the stream does not carry.

## Grounded progress

`PictureFrame` and its picture-only sink/pass were replaced, not aliased, by one ordered
`SemanticDraw` variant stream and `Semantic2dPass`. The pass renders pictures and solid rectangles
inside one SDL GPU render pass in submission order. A watched control renders red solid → green
picture → blue solid, proves blue at the overlap, then swaps the final two operations and proves
green plus a different frame hash. Separate controls cover clipped and half-alpha solids.

Both runtimes publish real `GC2D fill_rect` calls before retaining their original bodies. Bounded
title runs observed mixed picture/solid frames in both runtimes: six of 50 recomp semantic frames
and eleven of 400 decomp semantic frames.

Both runtimes now publish the immediate `J2DPicture::draw` family through the same picture command.
The active canvas comes from `J2DGrafContext::setup2D`, not a fixed-size guess; the direct resolver
uses the original function's position matrix, signed-16-bit destination extent, and orientation
arguments. Recomp captures after the retained body builds that matrix, while decomp publishes from
the corresponding source point before retaining GX emission. Close controls cover ordinary versus
transposed UV ownership and the native-layout sink. A guarded 400-present recomp Delfino run reached
the direct call and completed 3,019 operations without a GPU fault or capture refusal.

Both runtimes now publish individual `JUTResFont` glyphs as a distinct semantic family in that same
sequence. The shared resolver owns proportional/fixed width, bearing, advance, transformed corners,
retail fixed-point atlas coordinates, corner colours, font black/white remap, and clipping. The
recomp path retained 1,040 glyph calls in a guarded Delfino run and submitted them alongside 160
pictures and nine solids without regrouping. The decomp production-linked control exercises the
same value contract with actual encoded font-page bytes; its bounded title and stage-one runs did
not happen to reach resource-font glyph drawing, so live decomp glyph coverage is still unmeasured.

## Resolution

The ordered semantic stream now includes both remaining families. `J2DGrafContext::fillBox` emits
the matching signed-16-bit transformed gradient rectangle. `J2DWindow::draw_private` emits its
gradient, optional centered contents texture, four corners, and four edge strips through the shared
rectangle/picture commands. Both recomp and decomp retain their original bodies; close controls
cover ordering, transforms, colours, mirror bits, retail minimum-size culling, texture decoding,
and malformed input.

This resolves the missing-J2D-family ordering point. Visible composition and 3D/effect ownership
are separate capability gaps.
