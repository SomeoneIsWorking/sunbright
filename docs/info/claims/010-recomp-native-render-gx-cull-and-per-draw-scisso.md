---
id: C010
kind: claim
status: holds
created: 2026-07-29
tags: render
---

## Claim

recomp native render: GX cull and per-draw scissor were never implemented (cull hardcoded NONE, no scissor at all); aurora disagreed on 27409 of 29497 draws for cull and 675 for scissor. Both now ported and agree exactly (0 disagreements)

## Evidence

debug_journal 2026-07-29 iteration 9; oracle 'pix state' line shows SCISSOR 0, CULL 0

## What would falsify it

the oracle reporting a nonzero SCISSOR or CULL disagreement count
