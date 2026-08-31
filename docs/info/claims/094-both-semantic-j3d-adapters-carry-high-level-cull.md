---
id: C094
kind: claim
status: holds
created: 2026-08-30
tags: renderer,j3d,material,raster
depends: native-render/include/sunbright/native_render/j3d_material_state.h#j3d_texture_number_for_map, native-render/src/j3d_unlit_material.cpp#classify_j3d_raster_policy, native-render/src/model.cpp#transform_vertex, native-render/src/semantic_3d_pass.cpp#Semantic3dPass, sms-recomp/overrides/semantic_j3d_material_adapter.cpp#capture_guest_j3d_material_state, sms-boot/runtime/native_j3d_material_adapter.cpp#sb_native_capture_j3d_material_state
---

## Claim

Both semantic J3D adapters carry independent texture-slot/UV selection plus high-level cull, depth,
alpha-cutout, exact straight- or premultiplied-alpha blend policy, and linear view-depth fog into the PC-native renderer
without reading GX/FIFO raster state.

## Evidence

CPU controls cover compact and exact full-block policy families plus one-field refusals; the watched SDL GPU control distinguishes cull, alpha threshold, blend, and depth-write answers; guarded live runs exited 0 with 6,006 recomp and 2,278 native-decomp cutout/back-cull model submissions.

## What would falsify it

Either adapter reads GX/FIFO raster state, derives live texture slots from active colour-stage count,
selects the wrong decoded image or UV set, an exact supported policy maps differently across
layouts, a one-field custom policy is accepted, a guarded GPU control no longer produces the
known-different cull/alpha/blend/depth answer, a live perspective run submits zero supported
models, or an original draw body stops executing.

## Re-confirmed 2026-08-31 — linear view-depth fog

Both layout adapters copy the authored J3D fog type, start/end/near/far values, colour, and
range-adjustment state. The shared classifier admits ordinary linear fog without range adjustment
and refuses exponential, reverse, invalid, and range-adjusted variants. The renderer-neutral draw
carries only linear start/end planes and RGBA colour; transformed vertices carry view depth, and a
shared shader include applies the same fog after every supported PC material equation. A guarded
shipping-GPU control keeps the no-fog triangle red and turns the same triangle halfway through blue
fog into the expected colour-space-correct purple, with no kernel GPU fault. The live recomp census
also independently observed Mario's stable type-2 values (`199999..200000`, near 10, far 300000,
magenta-blue colour), falsifying the earlier assumption that reset fog always ends at its stored
coefficient far plane.

## Re-confirmed 2026-08-31 — premultiplied alpha

The full J3D pixel-policy classifier now maps only the exact source-one,
one-minus-source-alpha, depth-test-without-write tuple to an ordinary premultiplied-alpha blend
mode. Its CPU control distinguishes that tuple from straight source-alpha and changes one source
factor to prove the answers differ. The watched shipping-GPU control draws translucent red over an
opaque blue destination: premultiplied blending preserves more source red while matching the
straight-alpha destination-blue contribution, and the kernel watcher reported no fault. A guarded
120-present recomp audit submitted all 52 perspective-reached instances that had previously fallen
back solely on this blend tuple, raising native model coverage from 15,860 to 15,912. The matching
native-decomp audit aborted in retained GX code on issue 30's known illegal wrap value 3 before a
semantic summary, so it provides no decomp live-coverage evidence.

## Re-confirmed 2026-08-31 — source-alpha blending with depth writes

The full pixel-policy classifier now also maps the exact source-alpha/one-minus-source-alpha tuple
that retains depth writes to the existing ordinary PC blend and depth policy. The new fogged/tinted
two-texture classifier exercises that combination directly, and the guarded 120-present recomp
audit submitted all 200 reached instances instead of rejecting their raster policy. Other factor
tuples still refuse.

## Re-confirmed 2026-08-31 — independent texture slot and UV selection

J3DTevBlock2 loads both texture bindings regardless of whether one or two colour stages are active.
Both layout adapters now capture slot 1 independently of active-stage count, while the shared
classifier resolves stage zero's selected slot and carries only the chosen decoded image plus its
primary/secondary UV choice into the PC material. Controls cover a two-slot/one-stage big-endian
guest block, invalid slot refusal, distinct UV results, and the exact opaque replacement policy with
depth testing but no depth write. In the guarded recomp audit, the reached Mario material using
slot 1 and secondary UVs changed from 50 observed/0 accepted to a complete
50 observed/50 accepted/50 decoded/50 perspective-ready/50 submitted path. The native-layout
adapter compiled through the same shared contract; no decomp live-coverage claim is added while
issue 30 prevents the bounded audit from completing.
