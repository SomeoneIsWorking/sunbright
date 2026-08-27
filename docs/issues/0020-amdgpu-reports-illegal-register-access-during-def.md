---
id: 20
title: amdgpu intermittently reports illegal register access during default recomp + Aurora
status: investigating
symptom: During ordinary or diagnostic recomp + Aurora play, amdgpu can report Illegal register access in command stream after roughly 1 to 10 minutes, then time out and reset the GPU
tags: gpu,aurora,radv,recomp,random,reset
created: 2026-08-27
updated: 2026-08-27
---

## Current evidence

The 2026-08-26 recurrence is the current root-cause investigation. C072 narrows that historical
event to replay submit 1608 but does not identify a draw or packet. Issue 18 owns the missing
hardware-progress instrument. The causal-window submit's pipeline topology matches a completed
control; only dynamic command hashes differ, leaving dynamic state, resource lifetime, or
nondeterministic RADV behavior unresolved.

## Reproduction window

The user observes that the failure is random and may occur after roughly one to ten minutes. A
600-second normal-timing stage-1 control on 2026-08-27 reached submit 4,859 without a kernel fault.
Post-run kernel preflight was clean. The forced wall-cap intentionally cut off the final two submits,
so their missing callbacks are teardown evidence, not a GPU hang. One clean ten-minute sample does
not establish crash solidity and does not resolve this issue.

## Diagnostic dependency

Issue 18's `RADV_DEBUG=hang` lane is independent evidence because its synchronization may mask this
failure. A real driver activation is verified; a real hang report is not. The next recurrence on
normal timing should be read from the first kernel event and v2 flight file, while a recurrence in
the hang lane should additionally preserve or explicitly fail to preserve RADV's trace.

A separate 600-second synchronized run reached submit 5,403 without a kernel fault. RADV confirmed
`syncshaders, hang`; the timeout-final kernel barrier and subsequent preflight were clean. Because no
hang occurred, the watcher persisted `status: UNKNOWN`, `eligible=0`, and no artifact, which is the
correct non-success-shaped answer. Driver-inserted synchronization makes this lane
timing-incomparable and may hide the normal timing defect, so this second negative sample is not
crash-solidity evidence either.
