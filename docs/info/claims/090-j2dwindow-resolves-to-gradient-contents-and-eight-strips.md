---
id: C090
kind: claim
status: holds
created: 2026-08-30
tags: semantic-renderer,j2d-window,re
depends: native-render/src/window.cpp#resolve_window_layout, decomp/sms/src/JSystem/J2D/J2DWindow.cpp#J2DWindow::draw_private
---

## Claim

`J2DWindow::draw_private` resolves to a high-level gradient, optional centered contents texture,
four corners, and four edge strips with authored size gating, transform, UV/mirror, texture, colour,
and submission order.

## Evidence

Recovered `J2DWindow.cpp` is matching. Focused resolver controls cover signed-16-bit geometry,
matrix ownership, optional content, corner and edge order, and material values.

## What would falsify it

A matching DOL listing or controlled window draw differs in any gated geometry, transform, UV,
material, or ordering field.
