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

RE'd from GXSetTevColorOp/GXSetTevAlphaOp in decomp/sms/src/dolphin/gx/GXTev.c; 537 of 2816 enabled stages in a settled Delfino tick use it; pinned by sms-recomp/tests/tev_eval_test.cpp across all four widths

## What would falsify it

if tev_eval_test stops failing when compare handling is disabled, the tests are no longer checking it

## Path corrected 2026-08-12

Recorded as `tests/tev_eval_test.cpp`; the tests live under `sms-recomp/tests/`. Found by `tools/info/registry_paths.py`, which exists because a live entry pointing at a file that is not there reads as "the check is gone" to whoever consults this registry instead of searching.

The evidence was RE-RUN, not just re-pathed: `./build-sms-recomp/tev_eval_test` prints "tev_eval: all checks passed" and exits 0 today. That is a CPU-only unit test, so it could be verified during a session in which no GPU work was permitted.
