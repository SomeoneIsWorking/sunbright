---
id: C009
kind: claim
status: holds
created: 2026-07-29
tags: render
---

## Claim

recomp native render: the Delfino black background was a full-screen quad sampling an EFB copy destination with no render-to-texture in the port; fixed by implementing EFB copy -> texture and ordering copies by FIFO stream offset (edgeIoU 25.4 -> 32.2 at equal N=59, background crisp)

## Evidence

debug_journal/2026-07-23_native_texgen_and_texmap_bisect.md iterations 10-15; black-owner bisect reports INVALID (nothing black to attribute); scratch/screenshots/streamorder.png

## What would falsify it

the black-owner bisect finding a black-painting batch again, or the copy surface reading back as this port's own composited frame (feedback loop returning)
