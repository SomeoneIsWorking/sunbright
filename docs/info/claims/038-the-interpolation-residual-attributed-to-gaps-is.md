---
id: C038
kind: claim
status: holds
created: 2026-08-12
tags: 
---

## Claim

The interpolation residual attributed to GAPS is not a fixable pairing failure: almost every gap is 9+ ticks, i.e. an object that was culled and returned, where snapping is correct

## Evidence

aurora interp.cpp gap-length histogram over a 400-present Delfino run: gap 1 = 333,877 draws, gaps 2-4 = 55, gaps 5-8 = 93, gap 9+ = 898. Gap-tolerant matrix pairing (bound 4 ticks, alpha reweighted by spacing, camera divided out per the sample's own tick) recovers the 55; raising the bound to 8 would add ~93. Together 0.04% of paired draws. Supersedes the earlier reading of the same residual as '562 recoverable gaps', which was a count with no length distribution behind it.

## What would falsify it

a scene whose gap histogram is dominated by 2-8 rather than 9+, which would mean objects are being lost by the pairing table rather than by culling; or the culling behaviour changing so that objects stop disappearing for long stretches
