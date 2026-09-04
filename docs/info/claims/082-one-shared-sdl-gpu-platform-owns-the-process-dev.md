---
id: C082
kind: claim
status: holds
created: 2026-08-30
tags: renderer,gpu,architecture
depends: native-render/src/sdl_gpu_platform.cpp#SdlGpuPlatform::initialize_device, native-render/src/sdl_gpu_platform.cpp#SdlGpuPlatform::attach_presenter, native-render/src/sdl_semantic_frame_client.cpp#SdlSemanticFrameClient::shutdown
---

## Claim

One shared SDL GPU platform owns the process device and optional sole presenter while supporting
independent client targets; semantic image-cache state is published only after confirmed submission.

## Evidence

CPU controls cover copied call-table lifetime, the host-owned SDL-video precondition, device policy,
one window claim, two independent sRGB targets, planted failures, refusal to shut down with live
targets, and reverse teardown. GPU controls cover successful and canceled submissions, current-
revision residency, exact-clear empty output, and duplicate-consumption refusal.

## What would falsify it

A renderer client creates a second device or presenter, shutdown succeeds with live targets, a
canceled submission enters the resident cache, or a replaced image revision remains resident.
