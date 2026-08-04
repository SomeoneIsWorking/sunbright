---
id: C017
kind: claim
status: holds
created: 2026-08-05
tags: decomp,rng
---

## Claim

MsRandF() returned NEGATIVE values in (-1,0) on the host: rand()*(1.f/(RAND_MAX+1)) overflows int where RAND_MAX is INT_MAX (glibc), making the scale -4.66e-10. Every randomised quantity in the decomp was out of range, and TGraphWeb::getRandomNextIndex indexed mConnections[] from below, causing an intermittent Delfino SIGSEGV in TGraphTracer::moveTo. Fixed by computing the divisor in float under SMS_NATIVE_PLATFORM.

## Evidence

debug_journal/2026-08-05_msrandf_negative_rng_delfino_segv.md; control A/B 9/10 SIGSEGV without the fix vs 0/10 with it

## What would falsify it

if a host is used whose RAND_MAX is small enough that RAND_MAX+1 does not overflow, the unfixed expression is already correct there and this reasoning does not apply to it
