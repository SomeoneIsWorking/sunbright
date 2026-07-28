---
id: C008
kind: claim
status: holds
created: 2026-07-28
tags: 
---

## Claim

recomp native render: the entire named-texmap-units residual is draws sampling the zero dynamic-texture buffers; named+SBR_TEXMAP_SKIPZERO=1 scores 32.1/+0.748 (N=59) vs pinned 31.9/+0.747 (N=63), frame complete

## Evidence

scratch/logs/skipzero.log, scratch/bin/named_skipzero.png, journal 2026-07-28 final section

## What would falsify it

the zero buffers gaining a producer, or blend semantics of the strip/overlay draws being settled
