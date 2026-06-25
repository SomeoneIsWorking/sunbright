# 2026-06-25 — File-select choice state: "pitched scene / cubes too high" = option camera MID-PAN, not a render bug

## TL;DR (corrects the prior handoff's whole "remaining divergences" list)
The settled file-select capture (`SB_SEL_DUMP_SETTLED`) fired the instant the choice state was
entered (`selectBookmark unk10==2`). But the option camera (CPolarSubCamera) keeps **smoothly
panning into place for ~130 MORE present frames** after that. So every "settled" frame the prior
sessions analysed was caught **mid-pan**: the view is pitched ~16° DOWN, which shoves the horizon to
the top (thin sky), fills the screen with the tan beach, and pushes the A/B/C cubes up to the top of
the frame. None of those are render bugs — they are one wrong-timing capture. The prior handoff even
noted "the settled frames still ANIMATE (meandelta ~11)"; that animation WAS the camera pan, misread
as water/sparkle idle motion.

Fix: gate the settled dump on the camera view actually having **stopped moving**
(`sb_camera_view_settled()`), not just on `unk10==2`. With that, the first dumped frame is the true
settled file-select, and it **matches the GX oracle framing** (cubes lower-middle with A/B/C letters,
correct sky/sea/beach proportions). `scratch/frames/settled_fixed_01.png`.

## How it was proven (value-based, never eyeballed)
Repro = standard settled capture (`SB_SEL_DUMP_SETTLED` + continuous START pad + long idle tail).
1. **Camera readback (SB_CAM_DBG) at the dumped frame said the camera was correct** — eye=(1095,328,
   -13) target=(1148.5,413.8,-1008) fovy=40, the proven-correct establishing shot. This is the trap
   the prior handoff fell into: `[cam-oracle]` only prints every 60 frames, so its last value was
   from a frame BEFORE the choice-state dump, not the dumped frame itself.
2. **The cube draw matrix (SB_CHR_DBG) told the truth.** baseTR.t and the joint world matrix (anm0.t)
   are both correct = the cube world pos (840/1080/1320, 300, -1000). But `drawMtx.t = viewMtx*anm0`
   gave **view-space y = +252** at the dump frame, when the verified camera gives −113. So the VIEW
   matrix viewCalc multiplied by was wrong, not the cube.
3. **Dumping the live `j3dSys.getViewMtx()`** at the dump frame: r1=[-0.040,0.961,-0.273,-274.6] —
   pitched ~16° down, NOT the correct r1=[-0.005,0.996,0.086,-320.6]. And `C_MTXLookAt` ITSELF
   produced this pitched matrix at the dump frame (the camera's own pos/up/target were mid-pan).
4. **The view r1[3] translation was DRIFTING** across the dumped frames: −19 → −43 → ... → −280 over
   ~40 frames, heading toward the settled −320. A steady pan, ~3–8 units/frame. Dumping ~170 frames
   after `unk10==2`: cube drawMtx view-y swung +252 → **−99** (≈ the correct −113), and the rendered
   frame (`scratch/frames/settle_0170.png`) snapped to the correct oracle framing. Definitive: the
   end state is right; only the capture timing was wrong.

## The fix
- `native/src/scene_drive.cpp`: `sb_track_camera_settle()` records the per-frame view-matrix delta
  (max element move); `sb_camera_view_settled()` returns true once it's been < 0.30 for ≥8 frames.
  Called right after `C_MTXLookAt` builds `g_graphics.mViewMtx` each frame.
- `reference/sms/src/GC2D/CardLoad.cpp`: the `SB_SEL_DUMP_SETTLED` trigger now also requires
  `sb_camera_view_settled()` before requesting the dump. So the first dumped frame is truly settled.

## NEW capture recipe (supersedes the prior one)
Same as before but the dump now self-gates on camera settle, so any `SB_SEL_DUMP_SETTLED=N` value
gives N TRULY-settled frames (no more 90-frames-too-early). `scratch/frames/settled_fixed_01.png` is
the reference settled frame.

## Real remaining divergences vs the oracle (re-baselined at the TRULY settled frame)
The prior list (cubes-too-high, beach-fills-everything, purple windows, tiny Mario, black starbursts)
was almost entirely the mid-pan artifact. At the truly-settled frame the genuine residuals are:
1. **Two sun/spiral sprites** (the SMS sun logo, 52×52 IA4, decoded white via the new SB_IMM_PRIM_DBG
   texture dump → `scratch/frames/prim_66.png`) render as **black-outlined** between the slot labels.
   They blend wrong (intensity/vertex-color). Verify they belong at all vs the oracle.
2. **Diagonal teal/white stripes** on the sea/water surface under the cubes (oracle = smooth teal) —
   the shoreline-foam UV/blend residual already noted.
3. **Bright vertical light rays** from the top edge — sky god-rays look overdone vs the oracle.
4. **Mario** small at bottom-LEFT (oracle: front-centre, larger) — his low placement residual
   (option-scene floor collision not placed; `fileselect-mario-low-getheight-stub`).
5. Window FILL hue — now reads blue-ish at the settled frame (the "purple" was mostly the mid-pan
   tan background bleeding through the blend); re-check by vertex colour, don't eyeball.
NOTE labels show NEW/NEW/NEW vs oracle Corrupt/New/New = our save has 3 empty slots, NOT a bug.

## New tooling added this session (kept, env-gated)
- `SB_IMM_PRIM_DBG=N`: now also DECODES each textured 2D prim to `scratch/frames/prim_NN.ppm` (+ image
  ptr in the dump) — SEE the actual sprite content (proved the "starburst" is the white sun logo).
- `SB_CHR_DBG=1`: per-cube baseTR / joint anm0 / drawMtx (view-space) + the live j3dSys view matrix —
  the tool that separated "cube placed wrong" from "view wrong" from "view mid-pan".
