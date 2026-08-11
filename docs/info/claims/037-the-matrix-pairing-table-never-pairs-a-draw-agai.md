---
id: C037
kind: claim
status: holds
created: 2026-08-11
tags: 
---

## Claim

The matrix pairing table never pairs a draw against a sample older than one tick, so alpha's one-tick spacing assumption always holds for the matrix path

## Evidence

aurora interp.cpp Entry::stamp + g_pairedFresh/g_pairedStale: 333,348 of 333,348 pairings on a 400-present Delfino run used the previous tick's sample, 0 used an older one. Mechanism: begin_tick() clears g_cur, so the prev/cur swap cannot leave a two-tick-old entry where the previous tick's should be. Corroborated by the interp self-test's gap case, which draws a tag, skips a tick and draws again, and is REFUSED rather than paired against the older sample.

## What would falsify it

begin_tick() no longer clearing g_cur, or the planned gap-tolerant matrix pairing landing (which deliberately makes stale pairings legal and must then scale alpha by the spacing) — either makes the pairing-freshness line nonzero
