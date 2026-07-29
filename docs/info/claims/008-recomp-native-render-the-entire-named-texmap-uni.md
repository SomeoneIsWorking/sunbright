---
id: C008
kind: claim
status: falsified
created: 2026-07-28
tags: 
falsified_on: 2026-07-29
---

## Claim

recomp native render: the entire named-texmap-units residual is draws sampling the zero dynamic-texture buffers; named+SBR_TEXMAP_SKIPZERO=1 scores 32.1/+0.748 (N=59) vs pinned 31.9/+0.747 (N=63), frame complete

## Evidence

scratch/logs/skipzero.log, scratch/bin/named_skipzero.png, journal 2026-07-28 final section

## What would falsify it

the zero buffers gaining a producer, or blend semantics of the strip/overlay draws being settled

## FALSIFIED 2026-07-29

The residual was NOT the zero dynamic-texture buffers. Root cause (2026-07-29): a 6-vertex full-screen quad sampling the EFB COPY DESTINATION 0x80fea480; this port had no render-to-texture, so it decoded guest memory (legitimately zeros, since copy dests are serviced GPU-side and never written back) and multiplied the scene by black. SBR_TEXMAP_SKIPZERO scored well BY ACCIDENT — it fell back to unit 0 exactly on the draw that covers the screen, masking the defect. Fixed by implementing EFB copy -> texture and ordering copies by FIFO stream offset: edgeIoU 25.4 -> 32.2 at equal N=59.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
