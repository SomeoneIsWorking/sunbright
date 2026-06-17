# 2026-06-17 — Shredded skinned Mario at file-select (ngx renderer)

## Symptom
File-select (and any skinned model: Mario/NPCs): the native renderer (ngx) explodes the
skinned character into a cloud of triangles, while ALL static geometry (blocks, menu, signs,
beach, sky) renders fine. The Dolphin-GX oracle renders the SAME recomp state with a clean Mario.

## Verified facts (each via the new tooling, not eyeballing)
- **It is a RENDERER bug, not recomp.** `tools/gpshot --fs` (file-select zero-drift A/B, same
  present, `ngx_frame=564`): GX side = clean Mario, ngx side = shredded. Recomp's skinning math is
  correct; ngx mis-renders it. (docs/render_ab_harness.md explains why abshot2 is a valid oracle.)
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

## RESIDUAL (post-fix) — one bone (head/hat) floats
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
(`tools/gpshot --fs`: 29.5% ≈ baseline; m0 per-packet ≈ m1 g_posmtx, both shred). With ONE matrix
for all verts (`/ngxmtxsrc?m=2`, modelview) Mario is a COHERENT (squished) blob — so model-space
positions are fine; the shred is caused by the per-VERTEX matrix selection scattering verts.
→ Suspect: (a) the per-vertex `matidx` (PNMTXIDX byte, ngx_mesh.cpp:52 `vtx[i*vstride+0]`) is
mis-read per vertex (wrong byte / vstride miscount when TEXMTXIDX bytes are also present), so verts
get the wrong bone; or (b) the draw matrices need a different space than assumed (e.g. they are
NOT yet view-composed at the captured moment for this preview model). NEXT: dump, for ONE skinned
shape, the matidx histogram vs the slots the packets actually loaded, and verify a single vertex's
matidx selects the bone the GX oracle uses. Use `/ngxmtxsrc` live + a new per-shape vertex probe.

## Tooling built this session (live, no rebuild to USE)
- `tools/gpshot [--fs]` — robust one-shot zero-drift A/B (survives the sandboxed-Bash constraints;
  see docs/render_ab_harness.md).
- `/abshot2` reports `ngx_frame=N` (self-certifies the snapshot isn't stale).
- `/ngxmtxsrc?m=0|1|2` — LIVE skinned-matrix source A/B (per-packet / g_posmtx / modelview) with no
  rebuild; `/ngxshape` reports `pkt_applied`/`fallback` (cumulative).
- `SUNBRIGHT_DBG_PKT=1` — stderr dump of per-packet unkC tables + resolved matrices + base.
