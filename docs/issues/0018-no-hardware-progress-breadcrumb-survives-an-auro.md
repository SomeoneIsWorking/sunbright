---
id: 18
title: No hardware-progress breadcrumb survives an Aurora GPU hang
status: open
symptom: The flight recorder narrows the causal-window submit but packaged Dawn exposes no last-executed GPU command or PM4 breadcrumb
tags: gpu,aurora,radv,diagnostics,hang,reporting
created: 2026-08-27
updated: 2026-08-27
---

## Root cause

Aurora used to disable API validation and robustness and compile out GX debug groups in Release;
backend validation was disabled in every configuration. That coupling is removed by issue 19:
standard diagnostics now request Dawn's partial backend validation, retain API validation and
robustness, and encode GX debug groups independent of `NDEBUG`. The packaged Dawn immediately
reports that it was built without Vulkan validation layers, however, and exposes its Vulkan
instance but not the device, queue, or command buffer needed for `VK_EXT_device_fault`, AMD buffer
markers, or diagnostic checkpoints. The existing flight recorder therefore establishes submission
time and CPU-side semantics, not the last command processor trace point.

## What was tried / dead ends

WebGPU error scopes can capture API validation errors but cannot identify hardware progress and can
divert errors from the uncaptured callback. RenderDoc requires predicting and successfully ending a
capture, so it is not a crash-surviving hang trace. Debug labels alone are insufficient: Dawn only
forwards them through Vulkan debug utils in validation/RenderDoc configurations and does not retain
command-buffer label stacks in the device-loss callback.

## Current approach

Proper next diagnostic: an explicit guarded RADV hang lane using `RADV_DEBUG=hang`, never the
default. [Mesa's RADV hang-debugging documentation](https://docs.mesa3d.org/drivers/amd/hang-debugging.html)
says that it inserts trace markers and synchronization and writes
`radv_dumps_<pid>_<time>/trace.log` with the last command-processor-reached point plus pipeline,
shader, buffer-object, address-binding, VM-fault, and register evidence. Because synchronization can
mask the suspected lifetime/order defect, results from this lane are independent diagnostic
evidence, not normal-run equivalence. The watcher must preserve immediate process-group kill, claim
only a new exact-child-PID dump, and report missing/partial/permission-denied output as UNKNOWN.
UMR is absent and debugfs is root-only on this host, so initial reports will omit some wave/ring
detail.

### Note (2026-08-27)
Partial implementation 2026-08-27: guarded launchers accept only explicit
`SBR_RADV_HANG_DIAG=1`, preserve the effective `RADV_DEBUG` across `.env`, and the watcher snapshots
before launch then captures only a new exact-child-PID dump after stopping the process. I034
validates positive/negative collection and trace parsing. A real guarded stage-1 launch subsequently
proved that Mesa RADV 26.1.8 consumed the flag: the driver itself printed its costly-mode warning
and `Enabled debug options: syncshaders, hang`. That 30-present activation control completed cleanly,
so it could not prove hang-only trace production. The issue remains open until a real fault produces
and preserves a hardware-progress report.

The default `run.sh` path now owns the live watcher too. Normal windowed play has no diagnostic
time or present-count cap, but it still preflights the kernel journal, watches through the final
post-exit barrier, stops the exact game process group on the first GPU fault, and preserves the
incident evidence. `run.sh --diagnostic` adds the former conservative headless, muted, 60 Hz,
present-count, and wall-clock defaults. This closes the launcher-coverage gap; it does not close the
missing hardware-progress breadcrumb described by this issue.

The audit after that activation found a real preservation race: the external watcher kills the game
at the first kernel fault, while RADV writes its report inside that same process only after detecting
the hang. Immediate termination can therefore leave the exact-child report absent or partial. The
watcher must keep stopping the process group immediately for machine safety and report this limit
honestly; eliminating it requires a driver/device-fault path that survives the game process, or a
coordinated in-process quiesce path. The pinned Dawn package exposes the Vulkan instance but not its
device, queue, or command buffer, so an Aurora-only change cannot retrieve
`VK_EXT_device_fault` vendor data through the current backend boundary.

The watcher now attempts bounded exact-child RADV collection on every terminal path, not only a
matched fault, and writes a durable terminal report even when the answer is `UNKNOWN`. Its wall-cap
path also performs the same final kernel cursor barrier as a normal child exit after killing the
child and making a bounded reap attempt. CPU controls plant a dump on clean exit, omit it on
nonzero/timeout exits, and place a fault only in the final timeout barrier; all four answers disagree
as required. This closes missed-exit reporting but deliberately does not claim to solve the
in-process writer race above.

The first real every-terminal integration ran the synchronized stage-1 workload for 600 seconds.
RADV confirmed the diagnostic flags, the game reached submit 5,403 without a kernel event, and the
wall cap produced a durable terminal report with `status: UNKNOWN`, `eligible=0`, and no artifact.
Post-run kernel preflight was clean. This validates the real absent-dump answer and terminal wiring;
only a future real fault can validate hardware trace production.
