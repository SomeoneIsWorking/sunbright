---
id: C066
kind: claim
status: holds
created: 2026-08-22
tags: interpolation,tdl
---

## Claim

Continuously visible `TDLTexQuad` and `TDLColorTexQuad` members require stable per-quad identities
across dynamic batch membership changes.

## Evidence

A live stage-1 FLUDD observation reported 50 keyed arrays across 706 groups, zero unkeyed arrays,
zero layout mismatches, 472 paired arrays, four births, one correct reappearance, and zero
camera-only draws. A synthetic `A,B -> B,C` control followed B by identity.

## What would falsify it

A representative TDL observation reports an unkeyed array, layout mismatch, or continuously visible
camera-only draw, or the membership-change control stops following B.
