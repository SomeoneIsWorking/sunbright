# 2026-06-17 — Shredded skinned Mario at file-select (ngx renderer)

## Symptom
File-select (and any skinned model: Mario/NPCs): the native renderer (ngx) explodes the
skinned character into a cloud of triangles, while ALL static geometry (blocks, menu, signs,
beach, sky) renders fine. The Dolphin-GX oracle renders the SAME recomp state with a clean Mario.

## Verified facts (each via the new tooling, not eyeballing)
- **It is a RENDERER bug, not recomp.** `tools/render/gpshot --fs` (file-select zero-drift A/B, same
  present, `ngx_frame=564`): GX side = clean Mario, ngx side = shredded. Recomp's skinning math is
  correct; ngx mis-renders it. (docs/port/render_ab_harness.md explains why abshot2 is a valid oracle.)
- **Root cause class = multi-packet skinned-matrix reload.** `/ngxshape` shows skinned PNMTXIDX
  shapes have `maxnelem=11`: a J3DShape draws in up to 11 packets (J3DShapeMtxMulti), each RELOADING
  XF pos-matrix slots 0,3,6,… from its OWN useMtxIndexTable (`unkC`). ngx flattened all packets into
  one vertex buffer and transformed them with `g_posmtx` (the LAST packet's matrices) → every earlier
  packet's verts got the wrong matrix.
- **The seam is ASYNC — cannot be used for per-shape matrices.** `ngx_capture_indexed_posmtx`
  (Dolphin LoadIndexedXF) was assumed synchronous (memory `ngx-render-fidelity-gap` UPDATE-7); it is
  NOT. `seam_log_n=0` at every `capture(sh)` (verified) → the seam fires after the super-call. The
  global `g_seam_base` it records is a stale/other-shape base (gave garbage matrices, scale 60 /
  translation 57942 / astronomical).
- **The correct synchronous draw-matrix base = J3DSYS+0x104** (`j3dSys.mCurrentDrawMtx`).
  `J3DSys::setModelDrawMtx` (decomp) stores it there AND `GXSetArray(GX_POS_MTX_ARRAY, …, 48)`. So
  matrix m = `mem_r32(0x804045DC+0x104) + m*48`, read as 12 host floats — current at capture time.
  The old comment calling 0x104 "single-matrix only / wrong for indexed" was FALSE.
- **Per-packet resolution is now implemented and FIRES.** `capture()` reads each packet's
  `J3DShapeMtxMulti` (unk8=useMtxNum@+0x08, unkC=useMtxIndexTable@+0x0C), resolves slot j's matrix
  from the object-model base, stamps each vertex's packet, and `transform_eye` picks
  `g_pkt_mtx[vertex.packet][matidx/3]`. The matrices are SANE (pkt0 m00 = [0.109 0.698 -0.708
  -1379]; pkt1/pkt2 slot-0 correctly skipped where unkC==0xffff). Live counter:
  `pkt_applied=1,852,251 fallback=308,382` (86% of skinned verts take the per-packet path).
  The `unkC` tables read perfectly (pkt0=[7 45 44 46], 65535=skip sentinel).

## FIXED (mostly) — slots PERSIST across packets
The shred persisted even with sane per-packet matrices because GX XF pos-matrix slots are
CUMULATIVE: a packet (J3DShapeMtxMulti) reloads only its non-0xffff slots; verts that reference a
slot a later packet SKIPPED keep the matrix an EARLIER packet loaded there. Proven live
(SUNBRIGHT_DBG_PKT `[pktv]`): pkt1 verts have matidx=[0..27] (slots 0..9) but its unkC skips slots
0,1,5 — those verts must use packet 0's slot-0/1 matrices. My first cut treated each packet
independently → those verts fell back to g_posmtx → shred. Fix: maintain a RUNNING per-slot state
across the shape's packets (init from g_posmtx), update only each packet's loaded slots, snapshot
into g_pkt_mtx[packet] after each packet's loads. Result: Mario goes from a triangle cloud to a
COHERENT character (gpshot --fs: bottom-left region 51→36; ngx≈recognizable Mario).

## FIXED — the floating head/hat was a SINGLE-MATRIX sub-shape bug
The hat is a rigid (non-skinned) sub-shape of the model on the HEAD joint. `J3DShapeMtx::load()`
does `GXLoadPosMtxIndx(unk4, 0)` — it loads draw-matrix `unk4` (the shape's joint index) into XF
slot 0. ngx's single-matrix path used `mCurrentDrawMtx[0]` = draw-matrix INDEX 0 (the root), so the
hat rendered at the root and floated. Fix: for non-skinned shapes read `mMatrices[0].unk4`
(J3DShapeMtx.unk4 @ +0x04) and use `drawMtx[unk4]` (= mCurrentDrawMtx base + unk4*48). Map geometry
has unk4=0 so it's unchanged. Result: hat snaps onto the head; Mario fully matches the GX oracle
(gpshot --fs: 29.3%→26.1% overall, Mario region clean). **Skinned characters now render correctly.**

## (history) RESIDUAL (pre-hat-fix) — one bone (head/hat) floats
With the running-slot fix, `fallback=0` (all 2.27M skinned verts resolve a slot) and Mario's BODY
is coherent and correctly posed (crouch), verified by cropping the dock-Mario region from the SAME
abshot2 present (scratch/screenshots/mario.{ngx,gx}.png): oracle = head attached; ngx = the red
hat/head detached and displaced up-left, body fine. So one bone's pos-matrix is wrong (not a global
shred). Leads: (1) the head may use a slot never loaded by Mario's packets → run[] init from the
cross-shape g_posmtx is a stale matrix; (2) the floating hat could be a SEPARATE shape/model with
its own skinning; (3) per-bone NORMAL matrices are still single (J3DSYS+0x108) — lighting only.
NEXT: identify the head shape/slot and check whether it's shape 80ea09c0 or a separate model.

## (was) STILL OPEN — the per-vertex assignment is wrong
Despite applying sane per-packet matrices to 86% of skinned verts, **Mario still shreds**
(`tools/render/gpshot --fs`: 29.5% ≈ baseline; m0 per-packet ≈ m1 g_posmtx, both shred). With ONE matrix
for all verts (`/ngxmtxsrc?m=2`, modelview) Mario is a COHERENT (squished) blob — so model-space
positions are fine; the shred is caused by the per-VERTEX matrix selection scattering verts.
→ Suspect: (a) the per-vertex `matidx` (PNMTXIDX byte, ngx_mesh.cpp:52 `vtx[i*vstride+0]`) is
mis-read per vertex (wrong byte / vstride miscount when TEXMTXIDX bytes are also present), so verts
get the wrong bone; or (b) the draw matrices need a different space than assumed (e.g. they are
NOT yet view-composed at the captured moment for this preview model). NEXT: dump, for ONE skinned
shape, the matidx histogram vs the slots the packets actually loaded, and verify a single vertex's
matidx selects the bone the GX oracle uses. Use `/ngxmtxsrc` live + a new per-shape vertex probe.

## ★ RESOLVED (2026-06-17 pm) — the headed shred was NEAR-ONLY CLIPPING (not matrices)
The user kept seeing a shredded skinned model HEADED after the matrix fixes, while headless
file-select looked coherent. Reconciled by building a NUMERIC shred metric (the eye-space metric
was blind to it) and catching the actual spike frame:
- **Eye-space max-edge metric** (per skinned shape) stayed clean everywhere (all <500): the bone
  matrices ARE correct in camera space. Adjacent verts on a bone are close even when the model
  sits at the camera, so this metric CANNOT see a projection-space explosion.
- **NDC/screen max-edge metric** (front tris, w>1e-3) caught it: skinned shapes with NDC edges
  ≥40 (max 130252) during CAMERA TRANSITIONS (title→file-select fly-in). The shred is TRANSIENT
  (a few frames), which is why 8-frame headless sampling + single screenshots missed it — exactly
  matching the user catching it in one headed screenshot.
- **Auto-freeze-on-shred** (`SUNBRIGHT_NGX_SHREDFREEZE=<ndc-thresh>`) latches the snapshot on the
  first spike frame so /abshot2 captures it. (GOTCHA: the GX oracle XFB lags ngx's geometry
  publish during fast transitions — the auto-freeze A/B is NOT same-present mid-transition; trust
  the post-clip NUMBER + steady-state gpshot, not the auto-frozen GX side.)
- Root cause: the offending shapes are COHERENT (tight eye-bbox, nv=616) objects that straddle
  the near plane / sit off-screen during the transition. **ngx clipped ONLY the near plane;
  the GPU clips the full frustum.** The near-only clip interpolates a vertex onto the near plane
  between an in-front and a behind-camera vertex; that vertex lands far off-axis (NDC.x ≫ 1) and
  survived as a screen-spanning sliver Vulkan can't fully remove (a behind-camera or off-screen
  object should be clipped away entirely, like GX does).
- **FIX**: `ngx_clip_frustum_tri` — full 6-plane Sutherland–Hodgman clip in clip space
  (left/right/top/bottom/near/far), replacing the near-only clip in the emit loop
  (`SUNBRIGHT_NGX_NEARONLY` restores near-only for A/B). Unit-tested
  (`test_clip_frustum`: every output vertex provably inside all 6 planes). VERIFIED by number:
  **POST-CLIP emitted NDC-edge spikes ≥40 went 649 → 0** (max 135 → 0.35) across 30.9M emitted
  skinned tris at file-select and 44.7M at Delfino gameplay; steady-state gpshot --fs image clean.
  This is the title-logo/sun-rays/character "shear" class the journal kept attributing to matrices
  — it was the missing side-plane clip all along.
- Metric lives in `/ngxshape` (SHRED eye-space + NDC/screen + POST-CLIP buckets), always on.

## Tooling built this session (live, no rebuild to USE)
- `tools/render/gpshot [--fs]` — robust one-shot zero-drift A/B (survives the sandboxed-Bash constraints;
  see docs/port/render_ab_harness.md).
- `/abshot2` reports `ngx_frame=N` (self-certifies the snapshot isn't stale).
- `/ngxmtxsrc?m=0|1|2` — LIVE skinned-matrix source A/B (per-packet / g_posmtx / modelview) with no
  rebuild; `/ngxshape` reports `pkt_applied`/`fallback` (cumulative).
- `SUNBRIGHT_DBG_PKT=1` — stderr dump of per-packet unkC tables + resolved matrices + base.
