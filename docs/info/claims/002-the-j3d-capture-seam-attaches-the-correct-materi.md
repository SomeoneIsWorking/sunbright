---
id: C002
kind: claim
status: holds
created: 2026-07-28
tags: native-render
---

## Claim

The J3D capture seam attaches the correct material state to each drawable

## Evidence

0 of 936 / 953 / 902 snapshots disagree with the FIFO state at the shape's OWN draw. Correlate is the last draw at or BEFORE the snapshot, because ov_shape_draw runs the real J3DShape::draw first

## What would falsify it

if the correlate is taken as the first draw AFTER the snapshot it reports 47% mismatch — that number is an artefact, not a finding
