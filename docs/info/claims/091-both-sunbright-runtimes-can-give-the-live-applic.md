---
id: C091
kind: claim
status: holds
created: 2026-08-30
tags: renderer,semantic,presentation
depends: native-render/src/sdl_semantic_frame_client.cpp#SdlSemanticFrameClient::encode_last_sealed, sms-recomp/host/render_composition.cpp#RenderComposition::initialize, sms-boot/runtime/semantic_render.cpp#sb_semantic_render_initialize, tools/launch/run.py#parse_invocation
---

## Claim

Both Sunbright runtimes can give the live application window exclusively to the GX-free native J2D target while Aurora remains an offscreen reference.

## Evidence

Guarded production SDL GPU control: hidden startup refused, one known semantic frame presented, then one hidden-window frame completed and counted unavailable. Guarded recomp preview: 65/65 semantic frames presented, 0 unavailable, 286720 non-clear pixels. Guarded decomp preview: 400/400 presented, 0 unavailable, 2790 pictures plus 52 solids, 158038 non-clear pixels first seen on semantic frame 311. Offscreen audit control remained 65/65 complete with presented=0.

## What would falsify it

A preview run lets Aurora acquire/present the operating-system swapchain, reports a presented semantic frame without executing the SDL swapchain blit, fails to retain original game draw bodies, or either runtime cannot produce both a real presentation and non-clear semantic readback.
