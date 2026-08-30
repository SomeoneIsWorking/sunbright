---
id: 28
title: PC-native renderer omits J2DGrafContext filled boxes
status: resolved
symptom: Menu and overlay rectangles drawn through J2DGrafContext::fillBox still emit GameCube GX commands and never reach the shared semantic PC-native renderer.
state_items: S004,S005
tags: native-renderer,j2d,fillbox,gradient
created: 2026-08-30
updated: 2026-08-30
---

Root cause: both runtimes retained `J2DGrafContext::fillBox` only as GX vertex emission. The exact
retail function contract at `0x802eba70` narrows rectangle coordinates to signed 16-bit, transforms
four corners by `mPosMtx`, applies the graph context's four corner colours, and draws under its
scissor. The resolved path uses one renderer-neutral solid/gradient rectangle command in both
runtimes. Its dedicated source counter prevents existing GC2D fills from being mistaken for live
coverage of this seam.

### Resolution (2026-08-30)
Implemented one shared PC-native solid/gradient rectangle path for J2DGrafContext::fillBox in recomp and decomp. Exact GMSE01 signed-16-bit narrowing, mPosMtx transform, bottom-corner ownership, and target-pixel J2D scissor are covered by big-endian and production-linked native-layout controls; original GX bodies remain intact. The watched GPU control, 43/43 root/decomp tests, 30/30 recomp tests, Clang builds, format, and tidy pass. A guarded 180-present Delfino run exited zero but did not organically call fillBox, so no live-use claim is made; the new j2d-fill-boxes counter distinguishes future coverage from GC2D fills.
