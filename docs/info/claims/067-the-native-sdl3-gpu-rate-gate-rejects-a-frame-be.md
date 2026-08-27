---
id: C067
kind: claim
status: holds
created: 2026-08-24
tags: gpu,renderer,gx-compat
depends: sms-recomp/runtime/render/native_render.cpp#sbr_render_end, sms-recomp/runtime/render/native_gpu_admission.cpp
---

## Claim

The SDL3-GPU GX compatibility rate gate rejects a frame before any GPU work and the guarded sidecar completes without increasing the kernel amdgpu anomaly count

## Evidence

native_gpu_admission_test passes; guarded 15-present run completed with 9 rate rejections and unfiltered amdgpu count 42->42 on 2026-08-24

## What would falsify it

if sbr_render_end performs texture creation, transfer mapping, or submission before sbr_native_gpu_admit_frame, or a guarded run increments the kernel anomaly count
