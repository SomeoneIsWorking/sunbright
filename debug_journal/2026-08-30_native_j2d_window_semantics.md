# Native J2DWindow semantic composition

`J2DWindow::draw_private` is a high-level 2D ownership boundary above GX. The source in
`decomp/sms/src/JSystem/J2D/J2DWindow.cpp` is a matching decomp object, so its retained body is the
binary-grounded specification for both runtime adapters.

## Observable composition

A visible window submits, in order:

1. one four-corner-colour contents rectangle;
2. an optional contents texture centered over that rectangle by UV extent, rather than stretched;
3. four frame corners;
4. top, bottom, left, and right edge strips.

The edge strips reuse a one-dimensional row or column from the adjacent corner texture. Their UV
ownership is not equivalent to independently stretching four edge assets. Position writes narrow
to signed 16-bit before the parent and pane-global matrices are concatenated, matching the retained
GX vertex body. The outer rectangle's origin is not added to frame vertices; its width and height
set the local frame extent, while the supplied matrix and `mGlobalMtx` own placement.

The size gate uses the object's stored `mMinimumWidth` and `mMinimumHeight`, not a host-side
recalculation. A smaller window is a deliberate no-op and must be classified as culled before any
texture is decoded.

## Ownership

`native-render/src/window.cpp` is the one runtime-neutral geometry implementation. It produces the
existing generic gradient-rectangle and one-texture-picture commands, so the renderer does not gain
a window-specific shader or GX state. Recomp reads the retail big-endian object through
`j2d_window_adapter.cpp`; decomp reads its native object through `native_window_adapter.cpp` after
anonymous window fields were given semantic names. Both paths copy decoded image bytes during the
synchronous submission and retain their original game body.

The recomp seam is the existing widescreen `J2DWindow::draw_private` override. Semantic capture
happens after its caller-owned rectangle/matrix adjustment and immediately before its super-call,
so the semantic and retained paths observe the same final layout.

## Evidence and limits

The shared resolver control checks exact corners, centered contents UVs, all edge roles, every
mirror bit, the stored minimum-size gate, and malformed texture refusal. The recomp control parses
retail offsets and texture objects. The production-linked decomp control constructs a real native
`J2DWindow` and observes one gradient plus nine texture submissions through the shipping adapter.
Both full Clang test suites and the C++ quality gate pass.

A guarded 400-present decomp title audit completed and reported 9,271 semantic operations, but that
route organically called no `J2DWindow`; it is runtime-safety evidence only. A Delfino audit stopped
at an older retained-Aurora defect before any window submission: texture map 0 had illegal wrap-S
value 3 in `DrawBuf Indirect`. The same fatal is present in two 2026-08-05 logs, so it is not caused
by this work. Issue 30 records the core values and required root-cause trace. No sampler clamp or
fallback was added.
