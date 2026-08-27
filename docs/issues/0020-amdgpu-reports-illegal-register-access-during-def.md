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

The current Aurora audit found and removed one concrete asynchronous lifetime race: debug-marker
commands stored indices into a frame-global string vector that the game thread cleared before the
render worker necessarily consumed the packet. Markers are now owned by each render pass and are
deep-copied with replay. This is a real defect in current diagnostic builds, but the historical
Release incident did not enable those debug groups, so it is not claimed as the 2026-08-26 cause.

Probe v3 now records the exact untouched replay-source frame ID plus its selected pass-stream and
full-uniform hashes. Replay installation validates those hashes, the render worker independently
re-observes the installed packet before encoding, and the pre-submit worker gate re-hashes the final
selected command stream and expected uniform prefix before unmap after any intentional interpolation while proving the source
vertex/index/storage prefixes still have the same writer. Resolve fingerprints include source and destination resource
generations, the selected path, and the actual source sample count. The report explicitly excludes
standalone texture-copy FrameOps, attachment load/store and clear values, stencil clear, buffer
bytes, and host-added present/readback/ImGui commands; equality is not whole-command-buffer proof.

## Reproduction window

The user observes that the failure is random and may occur after roughly one to ten minutes. A
600-second normal-timing stage-1 control on 2026-08-27 reached submit 4,859 without a kernel fault.
Post-run kernel preflight was clean. The forced wall-cap intentionally cut off the final two submits,
so their missing callbacks are teardown evidence, not a GPU hang. One clean ten-minute sample does
not establish crash solidity and does not resolve this issue.

After the v3 lineage and marker-lifetime changes, a second normal-paced headless Interpolated 60
FPS control ran for the full 600-second user-reported window. It rendered 4,650 simulation ticks
and 4,650 retained presentations, reaching submit 9,311. The flight ring retained 171 successful
completions plus only the final returned submit whose callback was cut off by the wall-cap kill;
there were zero corrupt/torn/bounds records. The watcher's final barrier and an independent
15-minute kernel preflight were clean. This is another substantial negative sample, not proof that
the intermittent fault is fixed.

## Diagnostic dependency

Issue 18's `RADV_DEBUG=hang` lane is independent evidence because its synchronization may mask this
failure. A real driver activation is verified; a real hang report is not. The next recurrence on
normal timing should be read from the first kernel event and v3 flight record, while a recurrence in
the hang lane should additionally preserve or explicitly fail to preserve RADV's trace.

A separate 600-second synchronized run reached submit 5,403 without a kernel fault. RADV confirmed
`syncshaders, hang`; the timeout-final kernel barrier and subsequent preflight were clean. Because no
hang occurred, the watcher persisted `status: UNKNOWN`, `eligible=0`, and no artifact, which is the
correct non-success-shaped answer. Driver-inserted synchronization makes this lane
timing-incomparable and may hide the normal timing defect, so this second negative sample is not
crash-solidity evidence either.
