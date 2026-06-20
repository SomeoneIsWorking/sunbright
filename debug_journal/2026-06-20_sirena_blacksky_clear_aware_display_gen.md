# 2026-06-20 — Sirena Beach rendered BLACK in ngx → clear-aware display-generation filter (FIXED)

## Symptom
ngx (NGX_PRESENT=1) rendered the **entire Sirena Beach scene black** — only the HUD + FLUDD nozzle
visible — while the Dolphin-GX baseline (NGX_PRESENT=0, the oracle) rendered the full goopy beach
(stone wall, NPCs, parasols, Mario). Frame-EXACT A/B (same save loaded in both via `/loadstate`,
no drift): `tools/render/ab_oracle.sh scratch/sirena_beach.sav` reported 29% mean delta with the
bottom (floor) regions at 89–108/255 — i.e. ngx drawing nothing there.

Reachability: `SUNBRIGHT_STAGE=6 SUNBRIGHT_SCENARIO=0` fastboots into `sirena0.szs` (stage table in
`/data/stageArc.bin`: 0=airport 1=dolpic 2=bianco 3=ricco 4=mamma 5=pinnaBeach **6=sirena** 7=delfino
8=monte 9=mare 13=pinnaParco 14=casino …). Decoded the table with `sunbright-jingle --extract
/data/stageArc.bin` + a strings/`ScenarioArchiveNameTable` split (scratch/stagearc/).

## Root cause
The Sirena beach renders its **main scene into an offscreen EFB pass** (the "通常シーン描画ステージ"
normal-scene-draw / SMSVFilter_flicker indirection), then a LATER small offscreen GXCopyTex runs,
then the display composites. Per-frame copy sequence (`/efbcopies`):
- pass7  GXCopyTex clear=1  (0 shapes)
- pass8  GXCopyTex clear=1  (16 shapes)
- pass12 GXCopyTex clear=0  **(171 shapes = the main scene)** → dest 00fcc360
- pass14 GXCopyTex clear=0  (8 shapes, small overlay) → dest 810521a0

ngx's present `display_epoch` heuristic = **"highest tex-closed epoch"** (`ngx_j3d_shape.cpp`
`ngx_frame_publish`), then dropped batches with `epoch < display_epoch`. Here the highest tex-closed
epoch is **3** (pass14), so the present dropped everything below epoch 3 — including the **171-shape
main scene at epoch 2** → black. Proven by isolating epoch 2 (`/ngxrtfilter?on=0` + `/ngxepoch?keep=2`):
epoch 2 ALONE rendered the full beach.

This is the imperfection the code comment at `ngx_present.cpp` already named: the epoch model "DROPS
layers GX accumulated"; GPU truth is **the EFB accumulates and is RESET only by a CLEAR**. A later
*non-clearing* GXCopyTex must NOT demote the main scene.

## Fix — clear-aware display GENERATION filter (own the multi-pass EFB composite)
Switched the present's render-target filter from epoch to **generation**:
- `g_efb_gen` already existed (++ AFTER each clearing copy; shapes recorded `gen`). Added `gen` to
  **NgxRenderBatch** and set it at all 4 batch-creation sites (`ngx_j3d_shape.cpp`).
- Record `g_display_gen[g_cur] = g_efb_gen` at **publish** (`ngx_frame_publish`) — robust, NOT
  dependent on the GXCopyDisp override firing (ngx publishes at J2DScreen::draw, BEFORE CopyDisp).
- New accessor `ngx_snap_display_gen()`; present keeps only batches with `gen == display_gen`
  (`ngx_present.cpp`), via the pure, unit-tested helper `ngx_batch_displayed()`.
- Pure model extracted to `runtime/ngx/ngx_display_gen.h` (display gen = #clearing copies); shipping
  code CALLS it; `render_test` unit `display_gen` asserts the Sirena (gen 2) + plaza (gen 3) + no-copy
  sequences. 17/17 render_test PASS.

Why it's correct for both: Sirena last clear = pass8 → final gen 2 → scene(gen2)+overlay(gen2)+
display(gen2) all kept. Plaza last clear = pass9 → final gen 3 → 469-shape scene(gen3)+display kept
(matches the prior working behavior). The file-select file-panel-preview ghost stays fixed by the
SEPARATE sub-display-**viewport** filter (untouched).

## Verification (all frame-exact / baseline)
- **Sirena beach: BLACK → full scene rendered** (ab_oracle save=sirena_beach.sav). Residual delta is
  the **missing green pollution-goop overlay** (a separate, PARKED EFB-readback effect — see
  `delfino-lighting-wash`), NOT geometry; the floor/wall/NPCs now match the oracle.
- **Plaza 18.0%** (`oracle_ab.sh 15`) = exact baseline, no regression.
- **File-select 20.6%** = baseline (CLOF-ambient fix), no ghost-Mario regression.
- **Airstrip (stage 0)** renders correctly (Peach/Mario/plane/blue sky) — prior black-sky fix intact.

`/ngxorder` now prints `display_gen` + per-batch `gen` (kept consistent with the present filter).

## Post-fix stage sweep (verification — all main scenes render)
Swept `SUNBRIGHT_STAGE=2..9 SCENARIO=0` (fastboot, ngx). ALL render correctly:
2 Bianco, 3 Ricco, 4 Gelato, 5 Pinna, 7 Delfino, 8 Pianta Village, 9 Noki Bay (display_gen=10, many
generations — validates the gen model at scale). `display_gen == display_epoch` on all of them — the
two only diverge in the Sirena offscreen-composite pattern, so the gen change is targeted/safe.
⚠ **Pianta Village "black" was a FALSE ALARM** — the sweep captured it mid intro title-card WIPE
(letterbox fade); at 40s settled it renders fine (green bamboo/bridge, matches GX baseline). When
sweeping, always settle PAST the ~30 s level-intro wipe before judging "black". Don't re-chase
Pianta-black.

## NOT done / next
- The Sirena green **pollution goop** overlay is still missing (parked EFB-readback class). The
  Sirena **hotel mirror** (`TEfbCtrlTex 鏡描画ステージ`) is inside the hotel lobby (navigate from the
  beach) — still the canonical per-epoch *content* target (handoff task #1: render a non-display
  epoch into its own side buffer keyed by dest ea) for differently-VIEWED passes. This fix handles
  the common case (scene composited from its own accumulated generation); the mirror needs the
  side-buffer-per-epoch render and a lobby save to verify.
