---
id: C079
kind: claim
status: holds
created: 2026-08-28
tags: renderer,resources,j2d
depends: native-render/src/semantic_sink.cpp#submit_picture, native-render/src/image_decode.cpp#decode_image_rgba8
---

## Claim

A semantic J2DPicture submission must synchronously own the decoded image bytes and atomically pair
the command with the matching nonzero content revision before source storage can be reused.

## Evidence

Focused controls distinguish RGBA8, C4+IA8, and I8 content; stable and changed revisions; transient
source copying; short-palette refusal; and a missing sink that performs no allocation. The semantic
GPU picture control consumes the same owned image contract.

## What would falsify it

A source mutation is missed by the revision, a command can be accepted without its matching image,
or deferred decoding observes reused source storage.
