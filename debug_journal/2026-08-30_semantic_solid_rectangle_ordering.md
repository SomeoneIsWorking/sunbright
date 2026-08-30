# Ordered semantic pictures and solid rectangles

## Defect

The first PC-native semantic slice stored only `PictureDraw`. Adding solid fills as a second list or
render pass would have made a picture → fill → picture frame render as pictures → fills, destroying
the game's painter order while still producing plausible nonclear output. That would make visible
presentation structurally wrong, not merely incomplete.

## Ownership change

The picture-only frame, sink, and GPU pass were replaced by `SemanticDraw`, `SemanticSink`, and
`Semantic2dPass`; no compatibility alias remains. `SemanticDraw` is a variant of `PictureDraw` and
`SolidRectangleDraw`, and the collector enforces one total operation limit. The SDL GPU client
builds one vertex stream, opens one render pass, walks operations in exact sequence, and switches
between the textured-picture shader and a vertex-colour solid shader. Solid draws do not use a fake
white texture and no GX blend/register state enters the interface.

The packed RGBA8 conversion duplicated by the two picture adapters was moved to the shared semantic
core before the solid adapters reused it.

## Runtime seams

- Recomp: the existing sole override for guest `GC2D fill_rect` at `0x80140390` captures the final
  big-endian `JDrama::TRect` and r4 RGBA value after any widescreen mutation, submits the semantic
  operation, then calls `func_80140390`.
- Decomp: the source-level anonymous `fill_rect` in `GC2D/ScrnFader.cpp` publishes its native rect
  and colour before executing the unchanged GX body. Host allocations use the same scoped gate as
  picture decoding.

These are layout-local adapters. They share copied semantic values, not game objects or layouts.

## Controls and live evidence

The watched 16×16 GPU control renders red solid → green picture → blue solid. The overlap is blue;
moving green after blue makes it green and changes the frame hash. A wholly clipped solid is an
exact no-op, and a half-alpha solid blends rather than replacing the destination. The watcher saw
no kernel GPU fault.

The Clang adapter controls cover the same canonical TL/TR/BL/BR corner order in native and guest
layouts, packed colour, negative widescreen coordinates, host allocation scope, short guest reads,
and degenerate rejection.

Guarded launcher runs exited 0:

- recomp, 100 presents: 50/50 semantic frames completed; all 50 nonempty; six mixed-family frames;
  1,316 operations = 1,302 pictures + 14 solid rectangles; 1,302 images; first sampled frame had
  286,720 nonclear pixels.
- decomp, 400 presents: 400/400 completed; 350 nonempty; eleven mixed-family frames; 9,271
  operations = 9,207 pictures + 64 solid rectangles; 9,207 images; semantic frame 104 was the first
  sampled nonclear frame with 149,927 pixels.

## Remaining boundary

This is still an offscreen partial renderer. Text glyphs, J2D window contents/frame pieces,
`J2DGrafContext::fillBox`, direct picture calls outside the current screen scope, 3D, and effects are
absent. Mixed pictures and this one GC2D fill family prove the ordered mechanism; they do not prove
complete J2D ownership or authorize visible presentation.
