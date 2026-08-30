---
id: C082
kind: claim
status: holds
created: 2026-08-30
tags: renderer,gpu,architecture
depends: native-render/src/sdl_gpu_platform.cpp#SdlGpuPlatform::initialize_device, native-render/src/sdl_gpu_platform.cpp#SdlGpuPlatform::attach_presenter, native-render/src/sdl_semantic_frame_client.cpp#SdlSemanticFrameClient::shutdown, sms-recomp/host/render_composition.cpp#RenderComposition::initialize, sms-boot/runtime/semantic_render.cpp#sb_semantic_render_initialize
reconfirmed: 2026-08-30
verified_at: 2026-08-30 03:50:05
---

## Claim

One shared SDL GPU platform owns the process SDL device, optional sole window claim/presenter, and independent client targets; Semantic2dPass publishes image-cache state only after the caller confirms submission.

## Evidence

The SDL platform CPU control proves copied call-table lifetime, host-owned SDL-video precondition, SPIR-V device policy, one window claim, two independent sRGB targets, refusal to shut down with live targets, reverse teardown, and planted initialization/allocation failures. The watched semantic picture GPU control passes with no kernel fault and retains only the current revision. The migrated GX compatibility client completed a guarded 130-present run, then a 100-present exact-frame run whose identity control scored 100%/+1.000 and whose N=3 line joined frames 31/61/91; both exited cleanly.

## What would falsify it

A second device/window claim is created by a renderer client, platform shutdown succeeds while a live target references its device, a canceled/failed Semantic2dPass encode enters the resident cache, an old revision remains resident after a submitted replacement frame, or the guarded GPU controls fault.

## Re-confirmed 2026-08-30

Reverified shared SDL GPU device/window/event ownership after the mixed-stream change. Both Clang
builds and full test suites pass. Device-only CPU controls prove one process SDL device can exist
without a window claim and can attach the optional sole presenter later. The watched GPU client
control proves fenced submission, exactly-clear empty output, a nonclear planted mixed frame,
duplicate-consumption refusal, and reverse teardown. Both bounded live-runtime runs completed under
the kernel GPU watcher with no incident.
