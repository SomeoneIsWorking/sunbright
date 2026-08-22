---
id: C059
kind: claim
status: holds
created: 2026-08-22
tags: 60fps,interpolation
depends: extern/aurora/lib/gfx/indexed_interp.cpp#selftest, extern/aurora/lib/gfx/common.cpp#interpolate_recorded_frame
reconfirmed: 2026-08-22
verified_at: 2026-08-22 18:42:26
---

## Claim

The indexed-position interpolation arithmetic handles big-endian XYZ-f32 records and preserves non-position bytes

## Evidence

2026-08-22 shipping-path selftest: known 0->20 motion produced 5/10/15 at alpha .25/.5/.75, midpoint XYZ 10/20/30, and preserved the 4-byte tail; 720-present interpolated run invoked the control and exited clean

## What would falsify it

The control fails, an indexed record format outside its explicit XYZ-f32/stride contract is marked, or a live TDL draw shows a wrong in-between array

## Re-confirmed 2026-08-22

2026-08-22 updated shipping selftest passed big-endian 0->20 interpolation at .25/.5/.75, preserved the tail, rejected conflicting same-tick bytes, and paired B by identity across A,B->B,C; final live stage-1 control exited clean.
