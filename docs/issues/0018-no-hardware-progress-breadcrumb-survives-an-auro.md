---
id: 18
title: No hardware-progress breadcrumb survives an Aurora GPU hang
status: open
symptom: The flight recorder narrows the faulting submit but packaged Dawn exposes no last-executed GPU command or PM4 breadcrumb
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

## Resolution

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
validates positive/negative collection and trace parsing. Real `RADV_DEBUG=hang` activation and
hardware trace production remain unverified, so this issue is not resolved.
