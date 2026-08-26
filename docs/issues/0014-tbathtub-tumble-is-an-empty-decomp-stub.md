---
id: 14
title: TBathtub tumble is an empty decomp stub
status: resolved
symptom: TBathtub receives no retail angle-dependent X/Z impulse because tumble returns immediately
tags: decomp,re,movebg,stub
created: 2026-08-26
updated: 2026-08-26
---

## Root cause

`TBathtub::tumble(f32, f32)` remained an empty body after the upstream rebase. GMSE01
`0x801fb568..0x801fb5ec` gates on byte `0x29A`, scales strength by exact float `0.0001f`, converts
degrees to a JMath short angle, and adds sine/cosine components to fields `0x1E8` and `0x1F0`.

## Resolution

The decomp shipping member now implements that state transition. `platform_bathtub_tumble_test`
drives the linked function with asymmetric 0°/90° controls, pins the strength scale, and verifies
that the `unk29A` gate preserves every component.

