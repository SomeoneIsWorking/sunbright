---
id: 17
title: Replay draw ranges lack per-draw encode-time validation
status: investigating
symptom: A malformed replay DrawData range can pass aggregate frame high-water checks and reach Dawn encoding without a field-specific failure
tags: recomp,aurora,gpu,replay,validation,interpolation
created: 2026-08-27
updated: 2026-08-27
---

## Root cause

Replay preserves the recorded draw commands and original global vertex/index/storage regions while
interpolation can append replacement ranges for the live real emission. The existing replay checks
prove only aggregate reserved prefixes and copy cursors. Immediately before `gx::render`, individual
GX draw uniform/index/vertex/storage ranges and dynamic offsets are handed to Dawn without being
checked against the exact packet high-water marks. A corrupt `DrawData` can therefore evade the
aggregate checks.

This is a diagnostic coverage defect, not evidence that the captured submit 1608 contained an
out-of-range draw.

## What was tried / dead ends

Static audit found no actual out-of-range value in the v1 flight because that format retained only
aggregate counts/hashes. WSI/present, ImGui, Tracy queries, texture uploads/copies, persistent-arena
changes, encoder reuse, and EFB-cache eviction are absent or excluded from the causal headless
submit. The framebuffer readback ranges are internally valid and remain correlation only.

## Resolution

In progress: validate every provable draw range at the shipping encode seam before Dawn sees it.
Controls must independently corrupt each range class and prove field-specific rejection, plus pass
a boundary-valid replay draw. A range whose owner cannot be established from packet metadata must
remain named as missing coverage rather than receiving a guessed bound.

### Note (2026-08-27)
Partial implementation 2026-08-27: shipping pre-encode validation in
`extern/aurora/lib/gfx/replay_draw_validation.{hpp,cpp}` now rejects provable GX/RML vertex,
index, uniform, alignment, count/byte, replay-prefix, high-water, and interpolation-span defects;
13/13 focused controls pass. `observed_unchecked()` retains exact missing coverage: GX
per-frame/persistent indexed-array byte extents and RML dynamic binding extents are unprovable
because retained metadata lacks sizes/used masks. Issue remains investigating.
