---
id: 25
title: Runtime hosts did not encode sealed semantic J2D frames
status: resolved
symptom: Both runtimes published bounded above-GX picture frames, but no host activated the collector or submitted its sealed frame to the SDL GPU semantic pass.
state_items: S004,S005
tags: renderer,semantic,j2d,recomp,decomp,host
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

The SDL host layer coupled GPU-device ownership to a window claim, while the semantic bridge stopped
at a sealed CPU frame. Neither executable owned the target, pass, submit transaction, readback, or
reverse-order teardown needed to consume that frame without competing with Aurora's visible window.

## What was tried / dead ends

The first short stage-1 audit submitted frames but its only sampled frame stayed clear. That was kept
as a failed proof rather than interpreted as rendering. The title runs were extended until each live
producer supplied a known non-clear sample.

## Resolution

### Resolution (2026-08-30)
Device-only platform initialization, one shared fenced offscreen client, and runtime-local composition now consume each sealed sequence once. Guarded title runs completed 50/50 recomp frames with 1,302 draws and a 14,181-pixel nonclear sample, and 400/400 decomp frames with 9,207 draws and a 158,038-pixel nonclear sample; both exited without a kernel GPU fault.
