# 2026-06-22 — Own the sky draw (perform-list bypass); geometry flows, residual = camera matrix

Continues the lighting work (`2026-06-22_stage_lights_loaded_native.md`). With stage lighting
loaded, the next gap was the **black top-half sky**.

## Root cause of the black sky (MEASURED)
The sky model **"空グループ" (Sky Group)** is NOT a child of `通常シーン` — the GC renders it through
dedicated draw buffers **"DrawBuf Sky Opa/Xlu"**, sequenced by the master GX perform list
(`MarDirectorPreEntry.cpp`): set the sky buffers active (0x480) → entry the sky group (0x204) →
(later) draw. But `TPerformList::forEachPerform` AND's the call flag with each entry's stored flag
(`testPerform(param_4 & it->unk8)`), and the sky buffers' stored flag is **0x480** — frameInit |
setDrawBuffer, with **NO draw bit (0x8)**. So the perform-list path enters the sky into its buffers
but never draws them → top stays at the black clear. This is the same data-driven flag dispatch that
drops bit 0x8 from `通常シーン` (the blackbox `scene_drive` was built to bypass).

`J3DShape::draw` taps the capture for ANY drawn shape, so if anything drew the sky it'd be captured —
it wasn't, confirming the sky buffers are never drawn.

## What landed (`native/src/scene_drive.cpp` `drive_sky()`)
Drive the sky myself before `通常シーン::perform(8)` (so sky batches land behind the map), mirroring
`TSmJ3DScn::perform`'s buffer mechanics: copy view → `j3dSys.drawInit()` → frameInit the two sky
buffers → `setDrawBuffer(b0,0)/(b1,1)` → `空グループ->perform(0x20E)` → `b0->draw(); b1->draw()`.

The flag is **0x20E**, decoded from `TSky::perform` (Map/Sky.cpp) + `MActor::perform`:
- **0x2** → `TSky::setBaseTRMtx` (positions the dome relative to the camera) + `MActor::calcAnm`.
  THE missing piece: my first cut used 0x204 (no 0x2) → viewCalc built `draw = view × 0` → every sky
  vertex collapsed to one off-screen point (MEASURED: posMtx upper-3×3 all zero, ndc fixed at one pt).
- **0x4** → `MActor::viewCalc` (draw matrix = view × baseTR).
- **0x8** → `TSky` draws its procedural `GXDrawSphere` blue backdrop dome.
- **0x200** → `MActor::entry` into the active draw buffer.

## RESULT — geometry flows, but mis-positioned (verify-first, honest)
Sky now adds **10695→12717 scene verts, 27→31 batches** (frame-verified it IS captured + presented),
and projects to **non-degenerate, varying** NDC (was one collapsed point). So the sky pipeline is now
owned end-to-end. BUT the dome still isn't visible: the sampled sky verts land at **ndc y = 1.2–4.1**
(just above / beyond the top edge) at **z = 1.0** (the far plane).

### Residual = the known camera-matrix-source blocker (NOT a new bug)
`TSky::setBaseTRMtx` positions the dome from **`gpCamera->unk1EC`** (the camera's own stored matrix),
which differs from the **`C_MTXLookAt(pos,up,target)`** view I render the scene with. So the dome's
base TR is computed against a different camera frame than the view → it lands off the top. The scene
(palm/ground) renders fine because it uses my `C_MTXLookAt` view consistently; the sky breaks because
its positioning reads `unk1EC` directly. This is the SAME "active-camera view+projection SOURCE"
blocker flagged across prior sessions (memory `gameplay-perform-loop-2-ub-fixes`, the camera never
gets ctrl bit 0x1 so its real view/proj aren't the live ones). Also possible: the dome scale (100000)
exceeds the camera far plane → z-clip at z=1.0.

NEXT (the gate for the sky AND a faithful camera): resolve the camera matrix source — make
`gpCamera->unk1EC` / the live view consistent with what we render, OR derive `g_graphics.mViewMtx`
FROM `gpCamera->unk1EC` so `setBaseTRMtx` and the render view agree. Do NOT fudge the dome position
(NO BANDAIDS) — fix the shared camera source. ctest 28/28; no regression (scene unchanged, sky verts
render off-screen).
