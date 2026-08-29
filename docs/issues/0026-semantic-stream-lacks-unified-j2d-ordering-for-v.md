---
id: 26
title: Semantic stream lacks unified J2D ordering for visible presentation
status: open
symptom: The offscreen semantic target renders pictures, but presenting it would lose authored ordering against text, windows, fills, direct picture calls, and later 3D/effect families.
state_items: S004,S005
tags: renderer,semantic,j2d,ordering,presentation
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

The first vertical slice deliberately defined only `PictureCommand`. The game's actual J2D stream
interleaves pictures with text glyphs, window pieces, solid fills, and direct picture calls that do
not enter the current `J2DScreen::draw` context. A picture-only target therefore cannot replace or
overlay GX without changing authored draw order.

## What was tried / dead ends

Presenting the current offscreen target is ruled out: non-clear pixels prove liveness, not complete
frame ownership, and an overlay would hide or reorder semantic families the stream does not carry.

## Resolution
