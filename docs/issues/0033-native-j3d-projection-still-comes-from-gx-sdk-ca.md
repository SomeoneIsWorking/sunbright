---
id: 33
title: Native J3D projection still comes from GX SDK cache
status: open
symptom: Both semantic J3D adapters submit a normal projection matrix, but the recomp reads the mirror populated by GXSetProjection and native decomp calls GXGetProjectionv instead of receiving projection from the high-level camera owner.
state_items: S004,S005
tags: renderer,semantic,camera,j3d,recomp,decomp
created: 2026-08-30
updated: 2026-08-30
---

Find the game-semantic camera/projection owner used for J3D world draws in retail and native decomp. Publish the same renderer-neutral projection value from that owner in each runtime, preserve original bodies for A/B, and remove the semantic J3D adapters' dependency on the GX SDK projection cache. Prove perspective and orthographic/pass transitions with a planted control and guarded live runs; do not infer camera state from FIFO/XF registers or the GX compatibility renderer.
