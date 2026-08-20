---
id: C052
kind: claim
status: holds
created: 2026-08-20
tags:
depends: extern/aurora/lib/webgpu/gpu.cpp
reconfirmed: 2026-08-20
verified_at: 2026-08-20 21:27:30
---

## Claim

SB_HEADLESS runs the recomp renderer without X11, Wayland, or a WebGPU WSI surface

## Evidence

With no display server available, run-safe SBR_UI_SELFTEST=2 selected SDL offscreen, requested a Vulkan adapter with Compatible surface: false, rendered two RmlUi frames, exited 0, and logged zero amdgpu reset/fault events on 2026-08-20

## What would falsify it

A windowless run attempts a display driver or surface descriptor, or cannot create its offscreen render targets

## Re-confirmed 2026-08-20

After Aurora commit bafc344, a windowless run-safe SBR_UI_SELFTEST=2 selected SDL offscreen, requested the Radeon Vulkan adapter with Compatible surface: false, rendered two RmlUi frames, exited 0, and logged zero amdgpu reset/fault events
