---
id: C058
kind: claim
status: falsified
created: 2026-08-22
tags: 60fps,performance
depends: sms-recomp/overrides/native_frame.cpp, sms-recomp/runtime/devices/dev_gxfifo.cpp
falsified_on: 2026-08-22
---

## Claim

Native 60 is currently performance-limited in heavy Delfino intervals because a complete game/FIFO/render tick exceeds the 16.67 ms budget

## Evidence

2026-08-22 run-safe native-60 stage-1 700-present run: settled about 14.6 ms guest/FIFO plus 7.7 ms render per tick and 45-52 Hz; clean exit and zero GPU faults

## What would falsify it

A matched heavy-scene native-60 run sustains 60 Hz while reporting a complete tick below 16.67 ms, or the frame timing instrument is falsified

## FALSIFIED 2026-08-22

Wall-clock tick duration can show an end-to-end symptom but cannot attribute an internal bottleneck; contention already produced a false renderer regression. Optimization evidence is now deterministic subsystem work plus no-loss sampling.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
