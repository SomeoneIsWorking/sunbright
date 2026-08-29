---
id: C083
kind: claim
status: holds
created: 2026-08-30
tags: render,gx-compat,parity
depends: sms-recomp/runtime/render/render_compare.cpp#score_joined, sms-recomp/runtime/render/render_compare.cpp#on_aurora_frame, sms-recomp/runtime/render/native_render.cpp#sbr_render_readback, extern/aurora/lib/aurora.cpp#aurora_frame_sink_capture_frame_id
---

## Claim

The exact-frame GX-compatibility/Aurora A/B join measures the current recomp SDL3-GPU compatibility renderer at edgeIoU 28.17% and luma correlation +0.4110 over N=3 matched Delfino frames; this is compatibility evidence, not semantic-renderer progress.

## Evidence

2026-08-30: the guarded run-render path selected exact Aurora frame IDs 31, 61, and 91. The metric identity control first scored the non-degenerate frame 31 against itself at edgeIoU 100.0% and lumaCorr +1.000; the fixed N=3 line reported 28.17%/+0.4110, SBR_QUIT_AFTER stopped at 100 presents, and the guarded launcher exited 0.

## What would falsify it

Any GX compatibility output, Aurora frame-sink identity/scheduling, metric, scene/camera, dimensions, cadence, or sample-count change requires another same-contract run; any failed 100%/+1.000 identity control suppresses the verdict.
