---
id: C034
kind: claim
status: falsified
created: 2026-08-11
tags: interpolation,motion
falsified_on: 2026-08-11
---

## Claim

A fixed 100-world-unit-per-tick cutoff can distinguish ordinary object motion from interpolation
mispairing.

## Evidence

A controlled Mario movement trace reached 58.479 units per tick, initially suggesting a cutoff near
100. Pianta Village then supplied the required counterexample: one legitimate object moves 250–320
units per tick along a smooth continuous arc, producing 12,657 false refusals in the 100–1,000
bucket.

## What would falsify it

This claim is already falsified by the smooth Pianta Village motion. Per-object continuity, rather
than a global speed bound, is required; see C035.

## FALSIFIED 2026-08-11

The 10–100 observation remains valid for the measured player motion, but it cannot define a global
identity or continuity boundary.
