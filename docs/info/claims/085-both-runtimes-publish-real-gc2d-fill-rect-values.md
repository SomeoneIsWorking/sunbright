---
id: C085
kind: claim
status: holds
created: 2026-08-30
tags: renderer,semantic,j2d,ordering,recomp,decomp
depends: native-render/src/frame.cpp#SemanticFrameCollector::append, native-render/src/semantic_2d_pass.cpp#Semantic2dPass::encode, sms-recomp/overrides/widescreen_effects.cpp#run_fill_rect, sms-boot/runtime/native_solid_rectangle_adapter.cpp#sb_native_solid_rectangle_submit, decomp/sms/src/GC2D/ScrnFader.cpp#fill_rect
reconfirmed: 2026-08-30
verified_at: 2026-08-30 05:45:16
---

## Claim

Both runtimes publish real GC2D fill_rect values into one renderer-neutral picture/solid operation stream, and the SDL GPU pass preserves mixed-family submission order without consuming GX state.

## Evidence

Watched 16x16 GPU controls produce blue for red-solid then green-picture then blue-solid, produce green and a different hash when the final two operations are swapped, preserve an exact clipped-solid no-op, and blend a half-alpha solid. Guarded title runs exited 0 and observed mixed frames in both runtimes: recomp 6/50 with 1302 pictures plus 14 solids; decomp 11/400 with 9207 pictures plus 64 solids.

## What would falsify it

Falsified if either runtime publishes different final fill_rect bounds/colour than its retained body, mixed operation order changes between collection and pixels, any solid path consults GX/FIFO/Aurora state, the retained body is skipped, or the watched known-opposite ordering control stops producing the other colour/hash.

## Re-confirmed 2026-08-30

Reverified through the expanded mixed-family collector/GPU controls and guarded Delfino run: nine real GC2D solid rectangles remained ordered among 160 pictures and 1,040 glyphs; both retained solid bodies and adapter controls pass.
