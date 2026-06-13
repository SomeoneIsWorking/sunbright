# Camera / view-matrix system (SMS) — RE for 60fps camera interpolation

Goal: a rigorous map of how the gameplay camera produces the per-frame VIEW matrix,
where that matrix lives in guest RAM, and what reliably signals a scene cut, so the
60fps interpolation path can blend the camera on in-between fields and SKIP the blend
across a cut.

Primary sources (read, not guessed):
- `reference/sms/src/JSystem/JDrama/JDRCamera.cpp` — `TCamera` / `TPolarCamera` /
  `TLookAtCamera` / `TOrthoProj` (the JDrama camera base + view-matrix build).
- `reference/sms/include/JSystem/JDrama/JDRCamera.hpp` — class layout.
- `reference/sms/include/JSystem/JDrama/JDRPlacement.hpp` — `TPlacement` (`mPosition`@0x10).
- `reference/sms/include/JSystem/JDrama/JDRGraphics.hpp` — `TGraphics` (`mViewMtx`@0xB4,
  `mProjMtx`@0x74, `unk0` phase flags).
- `reference/sms/src/Camera/cameragc.cpp` — `CPolarSubCamera` (the real gameplay camera,
  `gpCamera`), incl. `CPolarSubCamera::perform`.
- `reference/sms/include/Camera/Camera.hpp` — `CPolarSubCamera` layout.
- `reference/sms/src/Camera/CameraDemo.cpp` — demo/cutscene cameras.
- `reference/sms/src/Camera/CameraWarp.cpp` — instant camera jumps (`warpPosAndAt`).
- `reference/sms/src/System/J3DSysFlag.cpp` — `TJ3DSysSetViewMtx::perform` (gfx → j3dSys).
- `reference/sms/src/System/MarDirectorPreEntry.cpp` — the main 3D perform list (phase).
- `reference/sms/include/JSystem/J3D/J3DGraphBase/J3DSys.hpp` — `J3DSys` (`mViewMtx`@0x0).
- `reference/sms/include/dolphin/mtx.h` — `typedef f32 Mtx[3][4]` (line 22).
- See also `docs/re_notes/perform_list_architecture.md` for the perform-list phasing.

Addresses are GMSE01 (US) from `reference/sms_gmse01_funcs.txt`. The decomp symbol maps
shipped in the submodule are GMSJ01/GMSP01 (JP/PAL) — addresses there DIFFER; only use
them for cross-checking sizes/names, not for our runtime addresses.

---

## 0. TL;DR object graph

```
gpCamera (CPolarSubCamera*)  @ guest global, type CPolarSubCamera
   : public JDrama::TLookAtCamera : public JDrama::TCamera
                                  : public JDrama::TPlacement : TViewObj

per frame, on the perform list (preEntry, MarDirectorPreEntry.cpp):
  1. push_back(camera1=gpCamera, 0x10)   -> CPolarSubCamera::perform(0x10, gfx)
         builds  unk16C (proj 4x4) and unk1EC (view 3x4) from unk124/unk148/mUp,
         copies them into gfx->mProjMtx / gfx->mViewMtx, and (bit 0x10) GXSetProjection.
  2. push_back(setViewMtx, 0x4)          -> TJ3DSysSetViewMtx::perform(0x4, gfx)
         MTXCopy(gfx->mViewMtx, j3dSys.mViewMtx)   <-- the LIVE view matrix the prompt reads
```

The live view matrix at the prompt's `0x804045DC` is `J3DSys::mViewMtx` (offset 0 of the
`j3dSys` global). It is a `Mtx` = `f32[3][4]` = 48 bytes, row-major (3 rows × 4 cols).

---

## 1. The camera classes

### 1.1 JDrama::TCamera and the look-at hierarchy (JDRCamera.hpp / JDRCamera.cpp)

```
TViewObj
  TPlacement                       (JDRPlacement.hpp)
      /* 0x10 */ TVec3<f32> mPosition       <-- camera EYE position
      /* 0x1C */ u16 unk1C
  TCamera : TPlacement, JStage::TCamera     (JDRCamera.hpp)
      /* 0x24 */ TFlagT<u16> mFlag
      /* 0x28 */ f32 mNear
      /* 0x2C */ f32 mFar
  TLookAtCamera : TCamera                   (JDRCamera.hpp)
      /* 0x30 */ TVec3<f32> mUp             <-- up vector
      /* 0x3C */ TVec3<f32> mTarget         <-- look-AT point
      /* 0x48 */ f32 mFovy
      /* 0x4C */ f32 mAspect
```

`mPosition` (eye) is inherited from `TPlacement` at **offset 0x10**, NOT 0x24. The
"native task" prompt offsets refer to the SMS camera (below), not these base offsets.

`TLookAtCamera::perform` (JDRCamera.cpp:65) is the canonical look-at build, but for the
gameplay camera it is OVERRIDDEN (see 1.3). It does, faithfully:
```c
C_MTXPerspective(gfx->mProjMtx.mMtx, mFovy, mAspect, mNear, mFar);
gfx->mNearPlane = mNear; gfx->mFarPlane = mFar;
C_MTXLookAt(gfx->mViewMtx, &mPosition, &mUp, &mTarget);   // eye, up, at
if (param_1 & 0x10) GXSetProjection(projMtx, GX_PERSPECTIVE);
```
`C_MTXLookAt` (0x80349f5c) writes a 3x4 view matrix from (eye, up, at).

`TPolarCamera::perform` (JDRCamera.cpp:21, US addr `802f71e8`) is a DIFFERENT base camera
(used elsewhere, e.g. menus) that builds the view from Euler angles + distance `unk44`
instead of look-at. `TOrthoProj::perform` (JDRCamera.cpp:106) builds an ortho proj +
translate-only view (2D). Neither is the gameplay camera.

### 1.2 The gameplay camera: gpCamera is CPolarSubCamera

`CPolarSubCamera* gpCamera;` — defined `src/Camera/cameragc.cpp:35`, set in the ctor
`gpCamera = this;` (`cameragc.cpp:86`), declared `extern` at `Camera.hpp:442`.

`class CPolarSubCamera : public JDrama::TLookAtCamera` (`Camera.hpp:171`). So the eye is
`mPosition`@0x10, target is `mTarget`@0x3C, fovy `mFovy`@0x48, aspect `mAspect`@0x4C,
near `mNear`@0x28, far `mFar`@0x2C — all from the bases.

Key SMS-specific fields (`Camera.hpp:341-360`):
```
/* 0x50 */ int mMode          camera mode (CAMERA_MODE_*; CAMERA_MODE_COUNT == demo)
/* 0x54 */ int mPrevMode
/* 0x124 */ TVec3<f32> unk124  FINAL eye position used to build the view this frame
/* 0x130 */ TVec3<f32> unk130  (= eye, copy)
/* 0x13C */ TVec3<f32> unk13C  PREVIOUS-frame eye (saved at top of perform)
/* 0x148 */ TVec3<f32> unk148  FINAL look-at target used to build the view this frame
/* 0x154 */ TVec3<f32> unk154  (= target, copy)
/* 0x160 */ TVec3<f32> unk160  PREVIOUS-frame target (saved at top of perform)
/* 0x16C */ Mtx44 unk16C        this frame's PROJECTION matrix (4x4)
/* 0x1AC */ Mtx44 unk1AC        PREVIOUS-frame projection (saved at top of perform)
/* 0x1EC */ Mtx   unk1EC        this frame's VIEW matrix (3x4)  <-- C_MTXLookAt output
/* 0x21C */ Mtx   unk21C        PREVIOUS-frame VIEW matrix (3x4)  <-- saved at top of perform
/* 0x64 */ u16 unk64            camera-kind flags: 0x200=gate demo, 0x400=demo off,
                                0x1000=jet-coaster
/* 0x2B4 */ CameraUnk2B4Struct* unk2B4   demo state; unk14 = remaining demo frames
```
The eye/target the game actually moves each frame are written to `mPosition`/`mTarget`
in the camera control code (e.g. `cameragc.cpp:198-249`), then copied to `unk124`/`unk148`
(the "final" pos/at) and the view is built from `unk124`/`unk148` (NOT directly from
`mPosition`/`mTarget`). So `unk124`/`unk148` + `mUp` are the inputs to `C_MTXLookAt`.

### 1.3 How CPolarSubCamera::perform builds the view (cameragc.cpp:951)

`CPolarSubCamera::perform` overrides `TLookAtCamera::perform`. (Not separately symbolized
in `sms_gmse01_funcs.txt` — it is the gameplay-camera vtable `perform` slot; cite the
source file.) Structure:

```c
if (param_1 & 1) {                       // CALC phase (movement sub-step)
    if (param_2->unk0 & 1) {             // first sub-step of the frame
        unk13C = unk124;                 // save PREV eye
        unk160 = unk148;                 // save PREV target
        unk1AC = unk16C;                 // save PREV projection (4x4)
        MTXCopy(unk1EC, unk21C);         // save PREV view matrix (3x4)
    }
    mUp.set(CLBConstUpVec);
    if (mMode != CAMERA_MODE_COUNT) {    // not a "simple demo" (reproduce) camera
        SMS_isOptionMap() ? ctrlOptionCamera_() : ctrlGameCamera_();
        calcFinalPosAndAt_();            // -> writes unk124 (eye), unk148 (at)
        fabricatedInline2();
    }
    if (mMode != CAMERA_MODE_COUNT) {
        C_MTXPerspective(unk16C, mFovy, mAspect, mNear, mFar);   // proj  -> unk16C
        C_MTXLookAt    (unk1EC, unk124, mUp, unk148);            // view  -> unk1EC
    }
    // ... demo update (updateGateDemoCamera_ / jet-coaster / updateDemoCamera_) ...
}

if (param_1 & 0x14) {                     // DRAW / set-graphics phase (bit 0x4 or 0x10)
    copy unk16C -> param_2->mProjMtx.mMtx (4x4)
    MTXCopy(unk1EC, param_2->getUnkB4())  // unk1EC -> gfx->mViewMtx (3x4)
    param_2->mNearPlane = mNear; mFarPlane = mFar;
    if (param_1 & 0x10) GXSetProjection(param_2->mProjMtx.mMtx, GX_PERSPECTIVE);
}
```

CRITICAL for interpolation: the camera's CALC (movement) work is gated on `param_1 & 1`,
runs at the FIXED game-logic rate, and writes `unk124`/`unk148`/`unk1EC` once per game
frame. The DRAW phase (`param_1 & 0x14`) just COPIES the already-built `unk1EC` into
`gfx->mViewMtx`. So between game frames, `unk1EC` (and therefore `gfx->mViewMtx` and
`j3dSys.mViewMtx`) are static — that is exactly why an in-between field looks stutter-free
only if WE blend.

Demo / "reproduce" cameras (mMode == CAMERA_MODE_COUNT) build `unk1EC`/`unk16C` inside
`updateDemoCamera_` instead (CameraDemo.cpp:74-76) using the SAME `unk124`/`unk148`/`mUp`
inputs, so the output location `unk1EC` is identical regardless of camera kind.

---

## 2. View-matrix flow: gfx->mViewMtx -> j3dSys global (TJ3DSysSetViewMtx, 0x80296a50)

`TJ3DSysSetViewMtx::perform` — `src/System/J3DSysFlag.cpp:18`, US addr **0x80296a50**:
```c
void TJ3DSysSetViewMtx::perform(u32 param_1, JDrama::TGraphics* param_2) {
    if (param_1 & 0x4)
        MTXCopy(param_2->mViewMtx, j3dSys.mViewMtx);
}
```
It copies `gfx->mViewMtx` (TGraphics+0xB4) into `j3dSys.mViewMtx` (J3DSys+0x0). Both are
`Mtx` = `f32[3][4]`, 48 bytes, row-major. **No transform** — a straight MTXCopy.

`j3dSys` global: `mViewMtx` is at offset 0 of `J3DSys` (J3DSys.hpp:102), so the j3dSys
global base == the view-matrix address. The prompt's RE puts that at **0x804045DC**
(US). That is the authoritative live view matrix used by all J3D drawing
(`J3DShape.cpp:25/32` does `GXLoadPosMtxImm(j3dSys.mViewMtx,...)` /
`GXLoadNrmMtxImm`, and `J3DTevs`/`J3DMaterial` concat against `j3dSys.getViewMtx()`).

### When in the frame it is set — the perform-list phase

From `src/System/MarDirectorPreEntry.cpp` (the MAIN 3D scene list, `unk34`/preEntry):
```
list->push_back(camera1, 0x10);          // gpCamera::perform(0x10) -> build + GXSetProjection
list->push_back(setViewMtx, 0x4);        // TJ3DSysSetViewMtx::perform(0x4) -> copy to j3dSys
... DrawBuf Sky / map / players / water ...
list->push_back(camera1, 0x10);          // re-asserted before WParticle / player group
list->push_back(setViewMtx, 0x4);        // re-copy to j3dSys
```
So per drawn frame: camera builds the view (phase bit 0x10), then `setViewMtx` (phase bit
0x4) latches it into `j3dSys.mViewMtx`, then the scene draws. The pair is issued TWICE
(once before the main scene, once before the player/particle group). `MarDirectorInitECT.cpp`
pushes `setViewMtx` with 0x4 in other lists (mirror/silhouette) too.

Phase-flag meaning for the camera/setViewMtx:
- `0x1`  = CALC sub-step (camera movement; runs N times per displayed frame at fixed rate).
- `0x4`  = "set graphics" / draw (copy built view into gfx; setViewMtx latches j3dSys).
- `0x10` = draw + also call `GXSetProjection` (issued for camera1 entries).
- `0x14` (= 0x4 | 0x10) is what TLookAtCamera/CPolarSubCamera test to enter their draw block.

`gfx->unk0` bits (separate from the perform phase, set by the director per sub-step,
see `perform_list_architecture.md`): bit 1 = "first sub-step of frame" (camera uses it to
snapshot prev-frame state), bit 2 = "second/last calc context".

---

## 3. Projection matrix path (GXSetProjection 0x80362c34)

The projection is built every frame by the camera into `unk16C` (4x4) via
`C_MTXPerspective(unk16C, mFovy, mAspect, mNear, mFar)` and copied into `gfx->mProjMtx`
(TGraphics+0x74) in the draw block; `GXSetProjection(gfx->mProjMtx, GX_PERSPECTIVE)` is
issued only on phase bit 0x10 (`cameragc.cpp:1009`, JDRCamera.cpp:52/76).

`GXSetProjection` US addr is **0x80362c34** (`reference/sms/src/dolphin/gx/GXTransform.c:39`).
The widescreen hooks in our runtime intercept here to widen the 3D projection for 16:9.

Does projection change per frame? YES, it is rebuilt every frame, but it only changes
VALUE when `mFovy`/`mAspect`/`mNear`/`mFar` change. In normal gameplay `mAspect` is
constant (config) and `mFovy` is constant except during specific zoom/demo logic
(e.g. gate-demo `updateGateDemoCamera_` chases `mFovy`, CameraDemo.cpp:94-110; demo BCK
animations can drive fovy). `mNear`/`mFar` are stable except demo cameras set
`mNear = mSLReproduceDemoNearClip` (CameraDemo.cpp:148).

Implication for interpolation: blending the VIEW matrix alone is correct in the common
case. Fovy can animate (zoom), so for full correctness the projection (or at least fovy)
should also be interpolated; but projection deltas are small and continuous except across
a cut, so a view-only blend is acceptable for v1 and projection interpolation is an
enhancement. There is NO need to interpolate `mAspect` (constant).

---

## 4. Scene cuts / camera demos — detecting a cut

A "cut" = an instantaneous change of viewpoint where the new view is NOT a continuous
move from the old. Blending across one shows a wrong intermediate view. Sources of cuts:

### 4.1 Instant jumps — warpPosAndAt (CameraWarp.cpp:9 / :29)

`CPolarSubCamera::warpPosAndAt(pos, at)` — US addr **0x800335d4** — teleports the camera:
```c
mPosition.set(pos); mTarget.set(at);
unk124.set(pos);    unk148.set(at);          // FINAL pos/at jump THIS frame
mInbetween->warpPosAndAt(pos, at); mInbetween->unk4 = 0;  // kill in-game smoothing
calcNowTargetFromPosAndAt_(pos, at);
mPreviousTarget = mCurrentTarget;
```
`warpPosAndAt(f32 ratio, s16 yAngle)` — **0x80033390** — computes a pos then calls the
vec form. After a warp, `unk124`/`unk148` differ discontinuously from last frame's
`unk13C`/`unk160`, and `unk1EC` (built that frame) differs discontinuously from `unk21C`.

### 4.2 Demo / cutscene cameras (CameraDemo.cpp)

- `startDemoCamera(name, offset, frames, f, bool)` — **0x80032a84** — switches into a
  scripted (BCK / map-tool) camera. If a demo BCK file exists it calls
  `changeCamModeSpecifyFrame_(CAMERA_MODE_COUNT, 1)` (mMode -> CAMERA_MODE_COUNT) and sets
  `unk2B4->unk14` = total demo frames. The first demo frame's pose is set by the BCK, an
  instant change from gameplay framing = a CUT.
- `endDemoCamera()` — returns to gameplay framing (`changeCamModeSpecifyFrame_(-1,1)`),
  another instant change = a CUT.
- `startGateDemoCamera` / gate-demo (unk64 & 0x200), jet-coaster (unk64 & 0x1000) — demo
  pose changes; cuts at start/end of the demo.
- `getRestDemoFrames()` — **0x800328e4** — returns `unk2B4->unk14` (frames left in demo);
  `isSimpleDemoCamera()` (CameraDemo.cpp:197) tests `unk2B4->unk14 > 0 || (unk2B4->unkC&1)`.
- Demos are queued by `TMarDirector::fireStartDemoCamera` (MarDirectorEvent.cpp:179) /
  `fireEndDemoCamera`, applied next frame in `direct()`. US addr of the director fire is
  `8029a23c`.

### 4.3 What state changes on a cut

On ANY cut (warp, demo start, demo end, stage/map change):
- `mMode` and/or `mPrevMode` change (e.g. -> or <- CAMERA_MODE_COUNT for demos).
- `unk124`/`unk148` jump discontinuously vs `unk13C`/`unk160`.
- `unk1EC` (the live view matrix, this frame) differs from `unk21C` (prev) by a LARGE
  delta (translation jump and/or rotation flip).
- `j3dSys.mViewMtx` (0x804045DC) therefore changes by a large delta vs its previous value.

### 4.4 Most reliable per-frame cut signal

Ranked, most-to-least reliable / robust to false positives:

1. **Large view-matrix delta** (recommended primary, render-side, engine-agnostic):
   keep our own copy of `j3dSys.mViewMtx` (0x804045DC) from the previous interpolated
   frame; each new game frame compute the delta between the new matrix and the previous
   one. A cut = (a) the camera EYE translation between frames exceeds a threshold relative
   to scene scale, OR (b) the forward/right basis vectors (rows of the 3x4) rotate by more
   than a threshold (dot of consecutive forward axes < cos(theta_max)). This needs NOTHING
   from the camera class and catches every cut source uniformly (warp, demo, stage change),
   because all of them end up as a discontinuous `j3dSys.mViewMtx`. Tune the translation
   threshold against per-frame camera speed (gameplay pans are bounded;
   `mInbetween`/`CLBChaseDecrease` smoothing in `ctrlGameCamera_` limits normal motion).
   NOTE the engine already exposes the matched pair to verify against:
   `unk124`(eye)/`unk148`(target) vs `unk13C`/`unk160`, and `unk1EC` vs `unk21C` — these
   are the game's own current-vs-previous, so the delta test can read guest-side
   `gpCamera->unk1EC` (0x1EC) vs `gpCamera->unk21C` (0x21C) directly instead of caching
   our own (both are the SAME data; prefer reading the engine's so a cut the game itself
   creates pre-draw is already reflected).

2. **mMode transition** (camera-class signal): a change of `gpCamera->mMode` (offset 0x50)
   between frames — especially to/from `CAMERA_MODE_COUNT` (demo) — flags demo cut
   boundaries precisely. Good as a CONFIRMING signal but not sufficient alone (a same-mode
   warpPosAndAt is a cut with no mode change; and some mode changes are smoothed by the
   in-between camera and are NOT visual cuts).

3. **Demo-frame edge**: `getRestDemoFrames()` (unk2B4->unk14, offset 0x2B4 -> +0x14)
   going 0->N (demo start) or N->0 (demo end). Precise for scripted cutscenes; misses
   plain warps.

There is no single dedicated "cut flag" in the camera; SMS relies on its own in-between
smoothing for non-cut transitions and just snaps for cuts. The delta-of-view-matrix test
(signal 1) is the only one that covers all cases, so it is the recommendation, optionally
gated/confirmed by signal 2 to suppress false positives from fast-but-continuous pans.

### 4.5 In-game camera smoothing is NOT frame-rate interpolation

`TCameraInbetween` (`mInbetween`@0x6C) and `CLBChaseDecrease`/`CLBChase*` in
`ctrlGameCamera_`/`ctrlNormalDeadDemo_` already smooth the camera at the GAME-LOGIC rate
(per game frame). That is independent of our 60fps in-between-field interpolation: it
shapes WHERE the camera is each game frame; we still need to interpolate BETWEEN those
game frames for the extra displayed field. Do not confuse the two. (Across a cut the
in-between is explicitly killed: `warpPosAndAt` sets `mInbetween->unk4 = 0`.)

---

## 5. CONCLUSION — what to blend, where, and the cut signal

**What to blend.** Blend the VIEW MATRIX. The clean source-of-truth inputs are the
camera's per-frame final eye/target/up, but the SAFE thing to interpolate is the built
view matrix itself, because all camera kinds (gameplay, demo, ortho-fallback) converge on
the same 3x4 `j3dSys.mViewMtx`. Two valid strategies:

- **(Preferred) Decompose-and-blend the view from eye/target/up.** Read the game's final
  inputs `gpCamera->unk124` (eye, +0x124), `gpCamera->unk148` (target, +0x148),
  `gpCamera->mUp` (+0x30); LERP eye and target, SLERP/renormalize up, then
  `C_MTXLookAt` the in-between matrix. This avoids the artifacts of naively LERPing matrix
  rows (which de-orthonormalizes the rotation). It exactly mirrors how the engine builds
  the matrix, so the blended field is a real intermediate camera pose.
- **(Acceptable) Blend the matrices and re-orthonormalize.** LERP the translation column
  and SLERP the rotation part (or nlerp+Gram-Schmidt) of `unk1EC`(prev=`unk21C`) — more
  fragile than the eye/target path; use only if eye/target reads are unavailable.

Optionally also interpolate FOVY (`mFovy`@0x48) for zoom demos; aspect (`mAspect`@0x4C) is
constant and must NOT be touched. Near/far are stable; leave them.

**Where the live view matrix is.** `j3dSys.mViewMtx` at **0x804045DC** (US), a
`f32[3][4]` (48 bytes, row-major) = `J3DSys+0x0`. It is written each frame by
`TJ3DSysSetViewMtx::perform` (0x80296a50, `if (phase & 0x4) MTXCopy(gfx->mViewMtx,
j3dSys.mViewMtx)`), which is fed by `gpCamera->perform` (phase 0x10) copying its built
`unk1EC` into `gfx->mViewMtx`. For an in-between field, write the blended 3x4 matrix into
0x804045DC (matching the layout) BEFORE the draw lists re-issue, and ALSO into
`gfx->mViewMtx`/`GXSetProjection` if re-issuing the camera1/setViewMtx pair. The
authoritative camera-side copies are `gpCamera->unk1EC` (this frame, +0x1EC) and
`gpCamera->unk21C` (previous frame, +0x21C); the eye/target/up inputs are `unk124`/
`unk148`/`mUp`.

**Most reliable cut signal.** A large per-frame view delta: compare `gpCamera->unk1EC`
(+0x1EC) against `gpCamera->unk21C` (+0x21C) each game frame (the engine's own
this-vs-previous view matrices) — equivalently compare eye `unk124` vs `unk13C` and
target `unk148` vs `unk160`. If the eye-translation magnitude or the forward-axis rotation
exceeds a tuned threshold, treat the frame as a CUT and SKIP interpolation for that field
(snap to the new view). Confirm/augment with a `gpCamera->mMode` (+0x50) change,
especially to/from `CAMERA_MODE_COUNT` (demo), and with `getRestDemoFrames` edges
(unk2B4 +0x14) for scripted cutscenes. There is no dedicated cut flag in the data — the
view-delta test is the only universal detector.

### Quick offset reference (from gpCamera, type CPolarSubCamera)
| offset | field | meaning |
|--------|-------|---------|
| 0x10 | mPosition | eye (TPlacement base; intermediate, prefer unk124) |
| 0x30 | mUp | up vector (input to C_MTXLookAt) |
| 0x3C | mTarget | look-at (base; intermediate, prefer unk148) |
| 0x48 | mFovy | field of view Y |
| 0x4C | mAspect | aspect (constant) |
| 0x28 / 0x2C | mNear / mFar | clip planes |
| 0x50 / 0x54 | mMode / mPrevMode | camera mode (CAMERA_MODE_COUNT = demo) |
| 0x124 | unk124 | FINAL eye this frame (C_MTXLookAt input) |
| 0x13C | unk13C | prev-frame eye |
| 0x148 | unk148 | FINAL target this frame (C_MTXLookAt input) |
| 0x160 | unk160 | prev-frame target |
| 0x16C | unk16C | proj matrix this frame (4x4) |
| 0x1AC | unk1AC | prev proj (4x4) |
| 0x1EC | unk1EC | VIEW matrix this frame (3x4) |
| 0x21C | unk21C | prev VIEW matrix (3x4) |
| 0x2B4 (+0x14) | unk2B4->unk14 | remaining demo frames |

| global | addr (US) | meaning |
|--------|-----------|---------|
| j3dSys.mViewMtx | 0x804045DC | LIVE view matrix (f32[3][4]); written by TJ3DSysSetViewMtx |
| TJ3DSysSetViewMtx::perform | 0x80296a50 | gfx->mViewMtx -> j3dSys.mViewMtx (phase 0x4) |
| GXSetProjection | 0x80362c34 | proj upload (widescreen hooks here) |
| C_MTXLookAt | 0x80349f5c | builds 3x4 view from eye/up/at |
| warpPosAndAt(Vec,Vec) | 0x800335d4 | instant camera jump (= cut) |
| startDemoCamera | 0x80032a84 | enter scripted demo camera (= cut) |
| getRestDemoFrames | 0x800328e4 | demo frames remaining |

---

## Uncertainty / caveats (flagged)

- `CPolarSubCamera::perform` and `TLookAtCamera::perform` are NOT individually symbolized
  in `sms_gmse01_funcs.txt` (inlined/devirtualized in the listing). They are the gameplay
  camera's vtable `perform` slot; the addresses above for warp/demo/setViewMtx ARE in the
  funcs file, but the per-frame `perform` itself must be reached via the camera vtable, not
  a named symbol. VERIFY at runtime by watching writes to 0x804045DC and to
  `gpCamera->unk1EC`.
- `0x804045DC` for `j3dSys.mViewMtx` comes from the prompt's prior RE (US). The decomp
  symbol maps in the submodule are JP/PAL and give DIFFERENT addresses — confirm the US
  `j3dSys` base at runtime (the J3DSys ctor `__ct__6J3DSysFv` MTXIdentity's mViewMtx, a
  good write-watch anchor: `J3DSys.cpp:28`).
- Cut-detection thresholds (translation / rotation) are NOT in the data — they must be
  tuned empirically against observed gameplay pan speed vs warp/demo deltas. Start
  conservative (skip-blend only on clearly large jumps) to avoid stuttering on fast pans.
- The frame-rate interpolation is ORTHOGONAL to the engine's own `TCameraInbetween`
  smoothing (which runs at game-logic rate). Blend BETWEEN consecutive game-frame views,
  not as a replacement for it.
