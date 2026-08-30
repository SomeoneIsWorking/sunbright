---
id: C084
kind: claim
status: holds
created: 2026-08-30
tags: renderer,semantic,j2d,recomp,decomp
depends: native-render/src/sdl_semantic_frame_client.cpp#SdlSemanticFrameClient::encode_last_sealed, sms-recomp/host/render_composition.cpp#RenderComposition::encode_semantic_frame, sms-recomp/overrides/native_frame.cpp#present_tail, sms-boot/runtime/semantic_render.cpp#sb_semantic_render_consume, sms-boot/runtime/frame_seam.cpp#sb_frame_present
reconfirmed: 2026-08-30
verified_at: 2026-08-30 04:46:12
---

## Claim

Bounded title runs of both runtimes activate the shared semantic collector and submit real nonempty J2D picture/GC2D-solid frames through the offscreen SDL GPU semantic pass while Aurora remains the visible GX path.

## Evidence

Recomp: guarded 100 presents, 50/50 semantic submissions completed, all 50 nonempty, 6 mixed-family frames, 1,302 pictures, 14 solid rectangles, and 1,302 images; the first sampled frame had 286,720 nonclear pixels. Decomp: guarded 400 presents, 400/400 completed, 350 nonempty, 11 mixed-family frames, 9,207 pictures, 64 solid rectangles, and 9,207 images; semantic frame 104 was the first sampled nonclear frame with 149,927 pixels. Both launcher exits were 0 with no kernel GPU fault. The production GPU control separately proves empty-clear, mixed planted-nonclear, and duplicate-consume refusal.

## What would falsify it

Falsified if submitted/completed counts differ, a bounded audit observes no nonclear sample, duplicate consumption succeeds, GX/FIFO/TEV state enters the semantic client, either launcher or GPU watcher fails, or changes to the client, host composition, frame seam, scene, or cadence are not rerun.

## Re-confirmed 2026-08-30

Reverified bounded live semantic collection after direct-picture integration: recomp Delfino completed 400/400 frames with 3,019 ordered operations and decomp stage 1 completed 400/400 with 643; both exited 0 under the GPU watcher, and the independent watched GPU control remained fault-free.
