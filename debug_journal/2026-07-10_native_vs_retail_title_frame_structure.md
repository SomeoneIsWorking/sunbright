# 2026-07-10 — First real native-vs-retail title-frame structural diff

Main-session hands-on. Produced the first apples-to-apples comparison of a **settled
title frame**: retail (cached FIFO `title_press_start_vi_stable.dff`, steady-state hold)
vs native (`SB_DRAW_DUMP_FRAME=2000`, retrace 4008, settled title). Both are past the
intro-camera settle.

## Tooling fix that unblocked this (aurora 9e00a51)

`SB_DRAW_DUMP_FRAME` matched `VIGetRetraceCount() == target` exactly. Retrace does **not**
advance by 1 per present at the title — `sb_frame_present` adds `retraces` (2+ NTSC fields)
per call and stalls during load loops — so it jumps over any absolute target and the dump
fired ZERO times for =1000/=3000. Fixed to count distinct retrace VALUES (a robust ordinal
frame index) and dump the Nth. Verified: frame 2000 → full 293-draw frame.

## The numbers

| | retail (steady) | native (frame 2000) |
|---|---|---|
| total draws | 1258 | **293** |
| color-writing (cU=1) | 71 | (not yet measured on native) |
| dome 202v | ×2 (cU=0, invisible) | ×2 |
| **52-vert backdrop-compositor block** | **×26** (SEG3, after EFB copy #2) | **×1** |
| mirror pass | 653 draws | 71 draws |

## Native's frame is structured as a double-draw (the "phase-1 ghost pass", CONFIRMED)

Native draws **every buffer twice** — first under `proj=O` (orthographic), then `proj=P`
(perspective):

```
  DrawBuf Sky Xlu      proj=O draws=9      DrawBuf Mirror Opa   proj=P draws=71
  DrawBuf MapOpa       proj=O draws=18     DrawBuf Mirror Xlu   proj=P draws=2
  DrawBuf MapXlu       proj=O draws=2      DrawBuf Sky Xlu      proj=P draws=9
  DrawBuf Mirror Opa   proj=O draws=71     DrawBuf MapOpa       proj=P draws=19
  DrawBuf Mirror Xlu   proj=O draws=2      <TLightDrawBuffer>   proj=P draws=12
  DrawBuf LensFlare    proj=O draws=11     DrawBuf StaticMapObj ShadowOpa proj=P draws=5
  <TLightDrawBuffer>   proj=O draws=12     ... then LensFlare(O) + ChrXlu(O) 2D overlay
```

The `proj=O` first pass is the documented **phase-1 ghost** — the buffers are flushed under
the **stale orthographic projection carried over from the previous frame's 2D-UI tail**,
before the real perspective pass. Retail has NO such duplicate: it rebinds perspective at
frame head (MANIFEST `title_press_start_vi_stable` finding) and draws each buffer once.
This is the first direct confirmation of the ghost pass in a real native frame, and it ties
the ghost to the projection-carryover symptom.

## Correction: the CLAUDE.md "dome behind-camera" cause is misdirected

Retail draws the 202v dome with **color_update=0** (measured: `title_per_draw_mtx.tsv` seq
5936/5937 posmtx=identity; cU=0 confirmed via `parse_fifo_dff` blend decode). The dome
**paints nothing visible** in retail's steady frame — the visible backdrop comes from the
71 color-writers, dominated by the 26× 52-vert block that samples the EFB copy #2 snapshot.
So whether native's dome resolves behind-camera is **irrelevant to the black backdrop**;
retail's dome is invisible regardless. Do not keep chasing the dome matrix for the
backdrop. (The `TSky::perform` `// TODO: match this awfulness` translation formula and the
`MTXScale(100000)` `GXDrawSphere` are separate and were a red herring — `GXDrawSphere(8,16)`
emits 34-vert strips, not the 202v draw.)

## Ghost pass LOCALIZED (viewport-proven, 2026-07-10 follow-up)

Added viewport to the draw dump (aurora). Native frame 2000 decodes unambiguously:

```
  proj=O vp=640x448  [GHOST]  Sky(9) MapOpa(18) MapXlu(2) Mirror Opa(71) Mirror Xlu(2) LensFlare(11) Light(12)  =125
  proj=P vp=256x256  [MIRROR] Mirror Opa(71) Mirror Xlu(2)                                                      =73   <- matches retail V_MIRROR
  proj=P vp=640x448  [WORLD]  Sky(9) MapOpa(19) Light(12) ShadowOpa(5) MapXlu(3) buf?(1) AfterIndirect(1)       =50   <- matches retail V_WORLD
  proj=O/P vp=640x448 [UI]    LensFlare(2 O +11 P) ChrXlu(32 O)
```

The ghost is the **first block: the ENTIRE 3D scene drawn under ORTHOGRAPHIC full-screen
(640x448)** — even the mirror is drawn full-screen ortho here, which is doubly wrong (the
real mirror is 256x256 perspective). The real perspective mirror + world passes follow and
match retail's viewports.

**Source: `unk40->push_back(drawBufferGroup, 8)` (MarDirectorSetupObjects.cpp:455).** unk40
is dispatched FIRST in TMarDirector::direct's RENDER branch (phase 1, before
mPerformListGX). It draws "Draw Buffer Group" (bit 0x8 = the whole 3D scene). In RETAIL this
is the single perspective world pass; in NATIVE it executes under the **stale ortho
projection** carried from the previous frame's GXPost 2D-UI tail because perspective is not
yet (re)bound in the fifo when unk40 runs — aurora defers the whole frame's fifo and drains
at present, and the phase attribution confirmed all draws drain together (phase reads 6 for
every draw at drain; emit-order is the real signal). Root cause of the ghost = the
projection state seen by unk40's draw is stale ortho, not the intended perspective. Fix
direction (unverified, next arc): establish the world camera/perspective projection before
unk40's Draw-Buffer-Group draw so the single scene pass renders under perspective (as
retail's does), eliminating the spurious ortho pre-flush — NOT by deleting unk40 (it is the
legitimate scene-draw pass in retail).

## What native must change (prioritized, from this data)

1. **Delete the phase-1 ghost pass** (the `proj=O` first flush of every DrawBuf). Zero
   retail counterpart; it double-draws the whole scene under a stale ortho projection.
2. **Port the mid-scene EFB-copy snapshot + the 26-draw compositor block** that paints the
   visible backdrop. The copy is triggered by `TEfbCtrlTex::perform` (bit 0x8 →
   `GXCopyTex`, `JDREfbCtrl.cpp`), built in `TMarDirector::initECT{Mir,Disp}`. NOT by the
   `TSnapTimeObj` "…Draw SnapTime" nodes — those are `TTimeRec` GPU-profiling timers
   (`unk14` inits 0 → no-op), a misattribution corrected here.
3. Native's mirror pass is impoverished (71 vs 653) — fewer mirror objects; secondary.

## Dead ends recorded
- Dome matrix / behind-camera / projection-carryover as *the backdrop cause*: FALSIFIED
  (dome is cU=0 invisible in retail).
- `MTXScale(100000)` GXDrawSphere vs retail identity posmtx: comparing different draws
  (sphere = 34v strips ≠ 202v draw). Not a divergence.
- `TSnapTimeObj` as EFB-copy trigger: FALSIFIED (it's a profiling timer).
