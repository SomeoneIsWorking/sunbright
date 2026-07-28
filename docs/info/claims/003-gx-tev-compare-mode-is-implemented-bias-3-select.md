---
id: C003
kind: claim
status: holds
created: 2026-07-28
tags: native-render
---

## Claim

GX TEV compare mode is implemented: bias==3 selects compare, the SCALE field carries the comparison width, the subtract bit selects == over >

## Evidence

RE'd from GXSetTevColorOp/GXSetTevAlphaOp in decomp/sms/src/dolphin/gx/GXTev.c; 537 of 2816 enabled stages in a settled Delfino tick use it; pinned by tests/tev_eval_test.cpp across all four widths

## What would falsify it

if tev_eval_test stops failing when compare handling is disabled, the tests are no longer checking it
