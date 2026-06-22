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

## DEAD-END tried (recorded so the next session doesn't repeat it)
Hypothesis: `unk1EC` is stale (the `param_1 & 1` block that computes it via `C_MTXLookAt` never runs
— MEASURED `[cam-perform] ctrl(b0)=0`), so `setBaseTRMtx` positions the dome from a stale matrix.
Tried: write our live view into it each frame — `C_MTXLookAt(gpCamera->getUnk1EC(), &pos,&up,&target)`
(the exact same call cameragc.cpp:989 uses). **RESULT: ZERO visible change** — the frame is byte-for-
byte the same black-top sky. So either `unk1EC` was already that value, or `setBaseTRMtx` isn't the
determinant. REVERTED (unverifiable change → not committed). The dome position is NOT gated on `unk1EC`.

## Refined diagnosis (the real residual)
The dome IS positioned (centered at the camera eye, per `setBaseTRMtx` extracting the eye from the
camera matrix). Its horizon ring (a y=0 dome vert, e.g. model (94839,0,-47295)) projects to **ndc
y≈1.237** — just above the top edge — at **z=1.0** (the far plane, but z=1.0 is IN-frustum, not
clipped). Working back: ndc_y = tan(pitch)/tan(fovy/2); with fovy=50 that puts the camera pitch at
**~30° down**. So with the current camera, the sky horizon sits just off the top and the visible sky
is a thin off-frame sliver → black top. Either (a) the camera's pitch/up is genuinely this steep for
the fastboot frame (then the sky really is mostly above-frame and the fix is elsewhere — e.g. the
GXDrawSphere backdrop should fill behind it), or (b) the camera view we build (`C_MTXLookAt` from
`JSGGetView*`) has a wrong pitch/up — the UNVERIFIED shared **camera-source blocker** (the camera
never gets ctrl bit 0x1, so its authoritative view/proj aren't proven faithful).

NEXT — needs a CAMERA ORACLE before more blind 5-min iterations (TOOLING-FIRST): capture the GC
camera view/proj from the Dolphin-hybrid (recomp) build at the same frame and diff against our
`C_MTXLookAt`/`C_MTXPerspective`, to settle whether our camera pitch/up/fovy/far are faithful. If
faithful → the sky positioning is correct and the black top is genuine for this angle (own the
GXDrawSphere backdrop or accept it). If not → fix the camera source (the long-open blocker), which
also unblocks faithful geometry placement generally. Do NOT fudge the dome (NO BANDAIDS).

ctest 28/28; no regression (scene unchanged, sky verts render off-screen for now). Committed state =
the sky-draw-ownership infrastructure (faithful `TSky` flag decode + buffer drive); visibility is
gated on the camera oracle above.

## UPDATE — camera oracle built (SB_CAM_DBG) + the camera-source blocker FIXED
Built a camera-state dump (`SB_CAM_DBG`, `scene_drive.cpp`): pos/target/up, fovy/aspect/near/far,
derived pitch + dome-equator ndc, and the view matrix rows. MEASURED at the plaza frame:
- `pos=(1095,328,-13) target=(267,828,239) up=(0,1,0) fovy=40 near=10 far=300000 pitch≈30°`.
- **`far=300000` ⇒ the sky dome (radius ~95000) is NOT far-clipped** (kills that hypothesis; the
  earlier "z=1.0" was just the dome being far vs near=10, in-frustum).
- The view matrix is self-consistent (forward = −row2 = (−0.83,+0.50,+0.25), looks up ~30°). The
  ground still shows at the bottom because world-origin geometry is far in −x = inside the fovy cone.

**Camera-source blocker ROOT-CAUSED + fixed**: `JSGGetViewPosition/Target` return the live
`mPosition`/`mTarget`, but the camera's CONTROL (`ctrlGameCamera_` → `calcFinalPosAndAt_`, which
UPDATES mPosition/mTarget + builds `unk1EC`/`unk16C`) is **entirely inside `CPolarSubCamera::perform`'s
`param_1 & 1` block** — and the perform-list never delivers ctrl bit 0x1 to the camera (MEASURED
`[cam-perform] ctrl(b0)=0`). So the camera was FROZEN. FIX: `scene_drive` now calls
`gpCamera->perform(0x1, &g_graphics)` each frame so the control runs (VERIFIED: fovy now updates
50→40, the control-computed value; no crash; scene unchanged). This is the long-open camera-source
blocker resolved — the camera tracks gameplay instead of being frozen.

## Sky residual now isolated (camera proven to run, far proven adequate)
With the camera running and far=300000, the sky dome STILL projects above the top (sampled sky verts
ndc y ≈ +1.2 … +4.1). So the residual is purely the **dome's vertical placement**: `setBaseTRMtx`
centers it at the eye, but its equator lands at/above the top edge instead of at the horizon, so the
upper hemisphere is off-screen → black. NEXT: instrument the dome's full projected ndc bounds (min/max,
not just shape[0]) and compare the equator ring's screen position to the ocean-horizon's; if the dome
is a thin band vs full hemisphere matters. The fix lives in how the dome's base matrix relates to the
view (re-derive `setBaseTRMtx` against our `g_graphics` view, OR the sky may want its OWN render with a
view that strips translation). The `GXDrawSphere` stub (`gx_fb_impl.cpp:296`) is still a parallel gap.

## FINAL ROOT CAUSE (s28c) — missing NEAR-PLANE CLIPPING (dome surrounds the camera)
By hand: `setBaseTRMtx[i][3] = eye[i]` (the formula reduces to the eye position), identity rotation ⇒
`draw = view·translate(eye)` ⇒ eye-space = `R·v` (pure rotation, NO translation). So the sky dome is
centered AT the camera (correct — that's how sky domes work); half its verts are in front (eye z<0),
half behind. The nvk raster is `VK_CULL_MODE_NONE` (double-sided) so culling isn't it. Built a per-
shape coverage probe (`[cov]`, capture loop): on-screen vert count + ndcY bounds + final colour. MEASURED
the sky shapes (drawn first, cc0=0700/0701, lit=0):
- shape#4 = the BLUE sky (col 0.01,0.50,0.86, 1800 verts, ntex=0) has **105 on-screen** verts but ndcY
  spans **[-31 .. +136]** — verts with |ndc|≫1 are the **near-plane-crossing (w≈0) / behind-camera
  (w<0)** artifacts. shapes #1/#2 (white, clouds) similarly span ndcY[-359..+9].
The dome SURROUNDS the camera, so its triangles straddle the near plane. The capture's `imm_project`
does the perspective divide BEFORE handing NDC to nvk, so behind-camera verts become garbage NDC that
Vulkan can no longer clip, and those triangles are discarded → black. The scene (palm/ground) renders
fine because it is entirely IN FRONT of the camera (no wrap, all w>0).

**FIX (the real next task): near-plane clipping for the capture path.** Either (a) pass CLIP-space
coords (x,y,z,w) to nvk and let Vulkan's near-plane clip handle it (stop pre-dividing in `imm_project`),
or (b) Sutherland-Hodgman clip each triangle against `w+z≥0` in the capture before the divide (the
`ngx_j3d_shape.cpp` WIP referenced in CLAUDE.md is exactly this for the recomp-era ngx path — port it
to the sms-boot capture). This is a general renderer capability: ANY geometry that crosses/surrounds the
near plane (sky, interiors, anything the camera is inside) needs it. Verify via `[cov]` (on-screen sky
verts should then RASTER blue) + the frame dump. Camera + far + dome-position are all PROVEN correct;
this is the sole remaining blocker for a visible sky.
