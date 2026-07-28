---
id: C007
kind: claim
status: holds
created: 2026-07-28
tags: 
---

## Claim

recomp: BP 0xE0-0xE7 RA writes carry R at bits 0-10 and A at bits 12-22; this port had the two SWAPPED, so every TEV colour register and every konst reached the shader with alpha in red and red in alpha. The fix is CORRECTNESS-ONLY — it does not move the score on this scene.

## Evidence

GXSetTevColor in decomp/sms/src/dolphin/gx/GXTev.c:130 — `SET_REG_FIELD(regRA, 11, 0, color.r)` and
`SET_REG_FIELD(regRA, 11, 12, color.a)`. Verified independently by the operator against that source,
not taken from the report that found it.

SCORE CLAIM WITHDRAWN: the reported 29.0 -> 32.0 compared `mean over 40` against `mean over 63`. One
post-fix run measures 28.9% (N=40), 31.9% (N=63), 32.9% (N=80) — the mean drifts upward with N as
the camera settles, so the rise is the sample count, not the fix. At equal N the change is noise
(29.0 -> 28.9, +0.719 -> +0.717) and the frame is visually indistinguishable.

## What would falsify it

If R and A ever stop being swapped in the SDK's own packing this is void. NOTE the trap this claim
was almost recorded with: any before/after cited for it must be at the SAME `mean over N`, because
this scene's mean rises ~4 points from N=40 to N=80 on its own.
