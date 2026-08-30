---
id: C090
kind: claim
status: holds
created: 2026-08-30
tags: semantic-renderer,j2d-window
depends: native-render/src/window.cpp#resolve_window_layout, sms-recomp/overrides/j2d_window_adapter.cpp#capture_j2d_window, decomp/sms/src/JSystem/J2D/J2DWindow.cpp#J2DWindow::draw_private
---

## Claim

Both runtime J2DWindow seams submit the matching high-level gradient, optional centered contents texture, four corners, and four edge strips through the shared semantic renderer before retaining their original GX bodies.

## Evidence

decomp/sms marks JSystem/J2D/J2DWindow.cpp Matching; the shared resolver, retail big-endian recomp adapter, and production-linked native J2DWindow controls pass; full Clang suites pass 44/44 and 31/31; the 400-present decomp title audit exits cleanly and reports zero window submissions on a path that does not draw them.

## What would falsify it

A matching DOL listing or controlled runtime window draw differs in size gating, signed-16-bit geometry, matrix ownership, UV/mirror ownership, texture/color material values, submission order, or retained-body execution.
