---
id: C081
kind: claim
status: holds
created: 2026-08-30
tags: renderer,architecture
depends: native-render/src/semantic_frame_bridge.cpp#SemanticFrameBridge::begin, native-render/src/semantic_frame_bridge.cpp#SemanticFrameBridge::seal, native-render/src/sdl_semantic_frame_client.cpp#SdlSemanticFrameClient::encode_last_sealed
---

## Claim

The semantic frame bridge is inert until explicitly activated, grants one exclusive sink lease, and
stores exactly one sealed sequence for one-time consumption.

## Evidence

Focused controls cover inactive begin/seal, exclusive acquisition, wrong-owner refusal, exact seal
storage, sequence increments, duplicate-consumption refusal, and teardown while collecting.

## What would falsify it

Collection activates without host composition, an unrelated owner can steal or clear the lease, a
sealed frame can be consumed twice, or teardown leaves a live collection behind.
