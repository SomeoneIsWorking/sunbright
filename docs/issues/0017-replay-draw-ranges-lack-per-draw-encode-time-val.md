---
id: 17
title: Replay draw ranges lack per-draw encode-time validation
status: resolved
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

Every replay draw now carries the metadata needed to prove its GPU-visible ranges before Dawn sees
them. GX `DrawData` retains the indexed-array used mask and all twelve byte ranges, then the encode
seam cross-checks each shader-visible offset and range end against the per-frame high-water mark or
persistent arena. RmlUi draw records retain the exact dynamic uniform-binding extents and validate
their alignment and ends against the uniform window. These fields are also included in the durable
GPU submit fingerprints, so a future incident report distinguishes submissions that differ only in
the newly covered references.

The old `observed_unchecked()` path was removed after its final four gap bits became impossible;
leaving a permanently-zero coverage registry would create a second, stale account of what is
validated. Focused Clang Debug controls pass 20/20, including deliberate per-frame and persistent
GX range overruns, a missing used-array extent, a mismatched shader offset, and an RmlUi dynamic
binding overrun. The shipping `aurora_gx` and RmlUi paths also compile in the project Clang Debug
tree.

This resolves the encode-time coverage defect. It does not prove that historical submit 1608 had
an invalid range: the v1 recorder did not retain these fields, so the original illegal-register
packet remains unidentified.
