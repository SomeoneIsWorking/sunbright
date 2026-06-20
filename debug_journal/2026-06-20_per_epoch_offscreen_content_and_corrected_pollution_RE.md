# 2026-06-20 — Per-epoch offscreen CONTENT (task #1) + CORRECTED pollution-goo root cause

## What landed (verified)
Generic **per-epoch offscreen-content** machinery in the ngx present, the long-deferred handoff
task #1, plus the published copy-event plumbing it needs:
- `NgxCopyEvent` (ngx_render_data.h) — a published per-frame EFB-copy event: kind/clear/epoch/gen/
  pass + **dest EA + dst dims + EFB src rect + format**. Filled by `ngx_note_efb_copy` (epoch/gen)
  and `ngx_set_last_copy_geom` (dims/fmt/srcrect, from the GXCopyTex override in
  efb_readback_native.cpp). Published via `ngx_snap_copyevts` (ngx_j3d_shape.cpp).
- `runtime/ngx/ngx_per_epoch.h` (pure, unit-tested `per_epoch` in render_test) — `wants_offscreen_
  content` (offscreen R8/fmt-0x28 copy with valid dest/dims), `batch_in_copy` (batch.epoch ==
  event.epoch), `argb_to_r8_coverage` (red channel replicated → I8/R8 sample expansion).
- `ngx_present.cpp render()` — for each wanted copy event, RE-RENDERS just that epoch's captured
  batches into the present target, reads it back, box-downsamples the EFB src rect → dst dims,
  converts to the R8 coverage scalar, and `sb_ngx_efb_store_copy(dest_ea, ...)` + invalidate. The
  pipeline is **verified end to end**: `[perepoch] queued/stored ea=80c72780 512x512` fires every
  frame on Sirena (SUNBRIGHT_STAGE=6). Gated by `SUNBRIGHT_NGX_NOPEREPOCH` (A/B). It is a **no-op
  for any scene without an R8 graffito copy** (`wants_offscreen_content` false) — plaza/file-select/
  airstrip unaffected (verified: default plaza renders 305 batches, Sirena renders the beach).

This is the correct, generic vehicle for own-the-framebuffer offscreen passes. It does NOT yet show
the Sirena goo — because of the corrected root cause below: the goo's coverage geometry is not in
any captured ngx batch, so the per-epoch render currently draws (almost) nothing.

## CORRECTED root cause — the handoff's premise was FALSE
Handoff/prior RE claimed "ngx captures the goo mask shapes via the J3DShape::draw tee (0x802e0390);
just render that epoch into the R8 coverage." **Falsified by `/efbcopies`:** the Sirena graffito copy
`dest=80c72780 fmt=40(R8) 512x512` reports **shapes_in_epoch=0** — ZERO J3DShape draws in that epoch.
The goo coverage is NOT drawn through the hooked J3DShape::draw.

Reading reference/sms instead of assuming:
- `TMarDirector::initECTGft` (MarDirectorInitECT.cpp) pushes the "落書きグループ" graffiti group into
  the GX perform list with flags `0x1000000` (the "graffito check") and `(i<<16)|0x2000008` (per
  layer), under a 512×512 `TOrthoProj` / "graffito" viewport, then a per-layer `TEfbCtrlTex`
  (`GX_CTF_R8`, mImagePtr = layer `unk54`) copies the EFB into the coverage.
- But `TPollutionManager::perform` (PollutionManager.cpp:93) does NOT draw joint models for those
  flags: `0x1000000` → `getCounterObj().countObjDegree()`, `0x2000000` →
  `getCounterLayer().countTexDegree(i)`. Only the plain (no-flag) perform calls
  `TJointModelManager::perform` (the normal J3D draw).
- `TPollutionCounterLayer::countTexDegree` (PollutionCount.cpp:602) is what actually RENDERS the
  coverage into the EFB: `drawBlack` + `drawTex(existing coverage)` + per-task `drawJointObjStamp`
  / `drawTexStamp` / `drawRevivalTexStamp`. Those use `drawShape(J3DShape*)` =
  `shape->getShapeDraw(k)->draw()` (the **inner** J3DShapeDraw, BELOW the J3DShape::draw tee) plus
  **immediate-mode GX quads** (`drawBlack`/`drawTex` = GXBegin/GXPosition). None of it goes through
  ngx's capture seam.

So the coverage is a **GPU feedback texture** (each frame: re-draw previous coverage + new stamps →
GX_CTF_R8 copy back), living only in VRAM. Confirmed: `unk54` RAM (80c72780) reads **all zeros**
(`/r?a=80c72780`), and Dolphin's EFB is empty under ngx present (the standing own-the-framebuffer
premise). ngx decodes textures from guest RAM → sees zeros → goo invisible.

## What the goo actually needs (the real remaining work)
ngx must RENDER the coverage itself (the side-buffer is the right destination — texture_for already
serves g_efb_side for unk54's EA, and the per-epoch machinery already stores+invalidates it). The
missing piece is **capturing / porting `TPollutionCounterLayer::countTexDegree`'s coverage render**
into ngx's batch pipeline:
- the stamp shapes drawn via `getShapeDraw()->draw()` (capture the inner J3DShapeDraw, or port the
  stamp geometry), AND
- the immediate-mode `drawBlack`/`drawTex`/`drawTexStamp` quads + the feedback (re-draw previous
  coverage) + the per-stamp task queue (`pushJointObjStampTask`/`pushModelStampTask`,
  `setTevColorInByStampType`, `calcViewMtx`) — PollutionCount.cpp is ~850 lines.
Once those draws land in the graffito epoch as ngx batches, the per-epoch render here produces the
coverage automatically and the goo appears. This is a sizeable native port (a feedback render-to-
texture), the genuine frontier — NOT a per-epoch wiring gap.

## Don't re-chase
- "The goo mask shapes are captured via J3DShape::draw, just render that epoch" — FALSE
  (shapes_in_epoch=0). The coverage is drawn via getShapeDraw()->draw() + immediate quads in
  TPollutionCounterLayer, a path ngx does not tee.
- Reading unk54 from guest RAM to get coverage — it's all zeros (GPU-only feedback texture).
- Letting Dolphin produce the coverage and reading it back — Dolphin's EFB is empty under ngx
  present; the original R8 GXCopyTex (which still runs for fmt 0x28) copies an empty EFB.

## Repro / tooling
- `/efbcopies` shows `shapes_in_epoch` per copy (the smoking gun: graffito copy = 0).
- `[perepoch]` DBG_EFB logs show the per-epoch render+store firing (pipeline verified live).
- Sirena: `SUNBRIGHT_STAGE=6 SCENARIO=0`, settle past the ~35 s wipe. GX baseline (the real goo) =
  `SUNBRIGHT_NGX_PRESENT=0` → bright green/yellow goo covering floor+water (scratch/screenshots/
  sirena_gx.gx.png). Canonical plaza = DEFAULT fastboot (no STAGE); STAGE=1/SCENARIO=0 does NOT
  boot to gameplay (black, frames=0 — a fastboot scene-state quirk, not a render bug).
