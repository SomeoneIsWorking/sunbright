---
id: C070
kind: claim
status: holds
created: 2026-08-26
tags:
depends: sms-recomp/runtime/render/native_render.cpp#sbr_render_tris, sms-recomp/runtime/render/native_raster_state.h
---

## Claim

The recomp native renderer drops GX_CULL_ALL triangle draws before any SDL GPU work while cull NONE, FRONT, and BACK remain submit-eligible

## Evidence

sms-recomp/tests/native_raster_state_test.cpp failed with exit 134 when the extracted shipping admission seam preserved the old always-submit behavior, then passed under clang++ -std=c++20 -Wall -Wextra -Werror after the fix. Aurora push_gx_draw and Dolphin VertexLoaderManager independently drop GX_CULL_ALL triangles.
The guarded stage-1 runtime reported zero cull-all draws, so it supplied no live-scene coverage and
is not evidence for this claim.

## What would falsify it

sbr_render_tris stops applying sbr_native_raster_submits_triangles before vertex/batch insertion, or the focused test no longer distinguishes cull ALL from BACK
