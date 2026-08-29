---
id: C084
kind: claim
status: holds
created: 2026-08-30
tags: renderer,semantic,j2d,recomp,decomp
depends: native-render/src/sdl_semantic_frame_client.cpp#SdlSemanticFrameClient::encode_last_sealed, sms-recomp/host/render_composition.cpp#RenderComposition::encode_semantic_frame, sms-recomp/overrides/native_frame.cpp#present_tail, sms-boot/runtime/semantic_render.cpp#sb_semantic_render_consume, sms-boot/runtime/frame_seam.cpp#sb_frame_present
---

## Claim

Bounded title runs of both runtimes activate the shared semantic collector and submit real nonempty J2D picture frames through the offscreen SDL GPU semantic pass while Aurora remains the visible GX path.

## Evidence

Recomp: guarded 100 presents, 50/50 semantic submissions completed, 42 frames carrying 1,302 draws/images, first nonclear semantic frame 9 with 14,181 pixels. Decomp: guarded 400 presents, 400/400 completed, 297 frames carrying 9,207 draws/images, first nonclear semantic frame 104 with 158,038 pixels. Both launcher exits were 0 with no kernel GPU fault. The production GPU control separately proves empty-clear, planted-nonclear, and duplicate-consume refusal.

## What would falsify it

Falsified if submitted/completed counts differ, a bounded audit observes no nonclear sample, duplicate consumption succeeds, GX/FIFO/TEV state enters the semantic client, either launcher or GPU watcher fails, or changes to the client, host composition, frame seam, scene, or cadence are not rerun.
