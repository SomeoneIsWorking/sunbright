---
id: C091
kind: claim
status: holds
created: 2026-08-30
tags: renderer,semantic,presentation
depends: native-render/src/sdl_semantic_frame_client.cpp#SdlSemanticFrameClient::encode_last_sealed
---

## Claim

The SDL semantic-frame client can exclusively own the application presenter while an independent
reference renderer remains offscreen.

## Evidence

A guarded SDL GPU control refuses hidden startup, presents one known semantic frame, then completes
one hidden-window frame while counting presentation unavailable. An offscreen control completes the
same semantic work with a zero presented count.

## What would falsify it

Another renderer acquires the operating-system presenter, the client reports a presented frame
without executing the swapchain blit, or offscreen mode performs a presentation.
