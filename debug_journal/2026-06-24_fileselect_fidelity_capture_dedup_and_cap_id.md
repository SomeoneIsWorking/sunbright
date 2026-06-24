# 2026-06-24 — file-select fidelity: per-frame capture dedup + the "TOO BA letters" are MARIO'S CAP

Continuation of the "perfect boot→file-select before Delfino" line (handoff
`scratch/handoff_fileselect_fidelity.md`). This session was diagnosis-heavy: it CORRECTS two
misdiagnoses in the prior handoff and lands one clean correctness fix + reusable tooling.

## Landed fix — single scene capture per VI present (the inter-frame duplication)
`sb_boot_capture_frame_begin()` (native/render/sms_boot_j3d_capture.cpp), called at the start of
`sb_boot_drive_scene()` (native/src/scene_drive.cpp). The capture buffer only cleared on the
first append AFTER a present-drain, so when `TMarDirector::direct()` runs more than once between
two VI presents (the logic loop outruns retrace under SB_TURBO), 2–3 **duplicate scene copies**
accumulated into one drained frame. Measured via the new `[batchdbg]`: the sea material
(key 224004d9) was captured **3×** in one present; after the fix, the inter-frame duplicate is
gone (deterministically one scene draw per present; `scene_batches` ~60→~40). The duplicate
copies interleave at the horizon and composite blended layers N× — a real contributor to the
dithered horizon band. Verified no regression: file-select still renders (frame dump boot_0120).

## CORRECTION #1 — the "lingering title-logo letters" are MARIO'S CAP (TMarioCap), not the title
The handoff's priority #1 ("chunky outlined SUPER MARIO SUNSHINE 3D letters … the title→select
transition doesn't fly/hide the 3D title-logo object") is WRONG. Hard evidence:
- The title logo is a **2D texture** (`j_title_bl.bti` in title.szs), there is no 3D letter model.
- The rainbow "T O O B A" shapes were traced by material key (c539bdd2 = a 5-stage TEV material,
  da39a890) → model pointer → **`TMarioCap::TMarioCap(TMario*)`** (mariocap.bmd, 8 shapes). Method:
  `SB_MODEL_TRACE` logs every BMD load's `J3DModelData` + caller return address; addr2line on the
  caller named the creator. (NOT the lens flare `sun_lensfx.bmd` and NOT the sun `model.bmd` —
  both have distinct model pointers; ruled out.)
- Root cause: `TMarioCap::perform` selects ONE of its 3 cap models (cap / metal-cap / diver-helmet)
  and hides the others **only under flag `& 2`** (MarioCap.cpp:123), and positions the cap from
  Mario's head-joint matrix. The native `scene_drive` drives the scene with **`perform(0x8)` only**
  (the draw bit) — never the `&2` setup/position pass that the real GX perform-list provides in
  sequence. So all 3 cap models stay visible at a default (≈origin → bottom-center) transform =
  the rainbow jumble. This is the SAME perform-list-bypass class as the sky/map draw (scene_drive
  already owns those); the cap is downstream of **Mario's model not being fully driven** in the
  option scene (his body isn't visibly rendering either). Fixing it faithfully = owning Mario's
  perform passes (&1 calc / &2 setup / head-joint skeleton) under the native scene drive — a
  substantial task, NOT a title-logo flyout. Left for the next session.

## CORRECTION #2 — the horizon "noise band" is z-overlap of multi-layer blend, not pure z-fight
- Confirmed it's GEOMETRY (persists with `SB_TEV_SOLID=1`, textures off), a dithered band of
  blue(sky)/teal(sea)/white(overlay) at z≈0.999. Depth is D32_SFLOAT with correct GX→Vk compare,
  so not coarse-precision z-fight.
- Per-batch dump (`SB_BATCH_DBG`): the opaque sky (b13, z=0.99990 const), the sea (z up to
  0.99916), a blended light-blue gradient (zw=0, z overlapping the sky), and a blended white
  overlay (bm=1/4/2, zw=0) all stack near the horizon. This is the documented **multi-layer-blend
  NO-ORACLE trap** (CLAUDE.md: "ti=10 additive / ti=9 premult white cloud layers … don't eyeball
  it"). The capture-dedup removed one compounding factor; the residual is faithfully un-chaseable
  without a pixel oracle (there is none for the sms-boot native path).

## Other findings
- `drive_map()` (マップグループ = map.bmd) draws the option-scene **ground/sand AND its own sea
  surface**, which overlaps the separate `seaindirect.bmd` sea drawn by 通常シーン → the sea is
  drawn twice per scene-draw. This is NOT a bug to remove (SB_NO_MAP washes the sand to white,
  proving map.bmd owns the sand); it's faithful map+sea layering.
- The bottom-left **beach diagonal stripes** are TEXTURE content (the sand .bti): the beach is
  smooth in `SB_TEV_SOLID` (R std 1.6) — almost certainly faithful, not an artifact.

## Tooling added (committed, env-gated, reusable)
- `SB_BATCH_DBG=1` (first scene frame) or `=N>1` (fire at present frame ≥ N): per-scene-batch
  NDC x/y/z span, depth/blend state, mean color + **color variance** (rainbow geometry = high
  cvar), shader key. In native/render/sms_boot_present.cpp.
- `SB_MODEL_TRACE`: `[modeltrace] BMDLOAD #n modelData=… caller=…` (J3DModelLoader.cpp) +
  `[modeltrace] createMActor 'name' → actor/model` (ObjModel.cpp). Identify any captured geometry
  by its creating model: match the renderer's per-batch model pointer to a load, then
  `addr2line -f -e build-native/sms-boot <caller>`.
- `SB_SEL_DUMP_N` now honored by the file-select dump trigger (CardLoad.cpp) — a large value
  gives a continuous frame window (the cap appears ~frame 70 as the option-camera intro-pan
  brings Mario into view; the old hard-coded 6-frame window missed it).

## Repro (unchanged; card already formatted → file-select; ~3 min, run in background)
    cmake --build build-native --target sms-boot -j$(nproc)
    pkill -9 -x sms-boot; S=""; for f in $(seq 200 12 1690); do S="$S ${f}:START $((f+6)):-"; done
    timeout -s KILL 200 setarch -R env SUNBRIGHT_DISC=scratch/disc/sms.iso SB_THP_FAST=1 SB_TURBO=1 \
      SB_HOST_ALLOC_CAP_MB=3072 SB_STAGE=15 SB_SCENARIO=0 SB_SEL_DBG=1 SB_SEL_DUMP=1 SB_SEL_DUMP_N=160 \
      SB_PAD_SCRIPT="$S" ./build-native/sms-boot > scratch/frames/fs.log 2>&1
    # The cap "letters" appear ~frame 70+ (boot_0100.ppm), not the early frames.
    # GOTCHA: the file-select reach is intermittent — some runs stick in the harmless
    # cardload setMessage id=0x1a (null src) idle at UNK13; just rerun.

## NEXT (for "perfect file-select")
1. **Mario in file-select** (the cap blocker): own Mario's perform passes / head-joint skeleton
   in scene_drive so TMarioCap's `&2` selects one cap model + positions it on Mario's head (and
   Mario's body renders). Verify the cap stops jumbling at bottom-center.
2. Horizon multi-layer blend: needs a pixel oracle for the sms-boot path before touching (trap).
3. hx_wipe type 10 (handoff #3, cosmetic transition wipe) — still open.
