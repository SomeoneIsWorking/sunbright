---
id: C085
kind: claim
status: holds
created: 2026-08-30
tags: renderer,semantic,j2d,ordering,re
depends: native-render/src/frame.cpp#SemanticFrameCollector::append, native-render/src/semantic_2d_pass.cpp#Semantic2dPass::encode, decomp/sms/src/GC2D/ScrnFader.cpp#fill_rect
---

## Claim

GC2D `fill_rect` contributes high-level bounds and colour to the same renderer-neutral ordered
stream as pictures and glyphs; the semantic GPU pass preserves mixed-family order without GX state.

## Evidence

Watched 16×16 controls produce blue for red-solid, green-picture, blue-solid; swapping the final two
operations produces green and a different hash. Controls also preserve a clipped no-op and blend a
half-alpha solid. Recovered `ScrnFader.cpp` establishes the title-level source values.

## What would falsify it

A retail trace disagrees with the recovered bounds or colour, collector order differs from pixel
order, or the solid path reads GX/FIFO/Aurora state.
