# 2026-06-20 — Airstrip (new-game first scene) black sky = headless EFB-readback artifact

User asked to compare file-select + the first new-game scene. SUNBRIGHT_STAGE=0 SUNBRIGHT_SCENARIO=0
fastboots into the Delfino Airstrip (loads data/scene/airport0.szs) — the new-game intro scene.

## What the comparison showed (headless, NGX_PRESENT 0 vs 1)
- FILE-SELECT (fs_oracle, 19% delta): ngx menu windows render too dark ("Select data." text nearly
  unreadable, should be bright white); overall darker. = residual file-select wash. Borders/icons/sky OK.
- AIRSTRIP: (1) the bright-blue daytime SKY renders BLACK in ngx; (2) HUD coin counter missing
  top-left; (3) overall darker. Characters/plane/dialogue/ground render fine.

## Black-sky root cause (traced, frozen + self-consistent)
At the black top-left, pixblend shows the sky base ti=9 renders correct bright blue (acc=(211,232,249)),
then **ti=42 darkens it to (16,18,19)** via two layers of frag=(0,0,0,a0.72), blend SRC_ALPHA/INV_SRC_ALPHA.
- ti=42 = the **BathWaterManager ocean reflection/distortion** (Map/BathWaterManager.cpp: GXSetTexCopyDst(
  wd>>1, ht>>1, GX_TF_RGB565) + GXCopyTex → a half-res 320×224 RGB565 framebuffer copy at **80f94fe0**;
  + a 64×64 I8 noise map; POS-projected screen UV). At the airstrip you stand on a platform over the SEA,
  so the distant ocean (d=0.996) covers the upper screen in FRONT of the sky (d=1.000) — legitimate.
- ti=42 samples 80f94fe0 = **black** at the sky/horizon corner → darkens the blue sky to black.

## Why 80f94fe0 is black = HEADLESS HARNESS LIMITATION (not a headed bug)
- ngx's GXCopyTex writeback (efb_readback_native.cpp copytex_writeback @0x8035ee5c) serves EFB→tex copies
  from ngx's own rendered scene color readback (g_efb_color, ngx_present.cpp sb_ngx_efb_copy_region).
- **g_efb_color is never populated in headless**: every fmt4/5 copy fails with "[efb-wb] NO FRAME", no
  color-readback stats line, and `/ngxpresentlive frames=0` STAYS 0 over time. The per-frame ngx PRESENT
  (sb_ngx_present_xfb, which does the readback) DOES NOT RUN in headless — it only renders ON-DEMAND per
  /abshot (abshot bumps frames 0→1). So the EFB color readback is never maintained → all EFB-copy-read
  effects (ocean reflection, sun occlusion 48×48@803f4440) read black.
- Warming with repeated /abshot DID get the copies served (`[efb] CopyTex served ea=80f94fe0 320x224
  fmt=4 nz=71680/71680`), but the served content is itself dark (the reflected sky is already dark) and
  the airstrip sky STAYED black: a CIRCULAR feedback (dark reflection → dark framebuffer → dark copy).
- HEADED (run.sh): the Presenter calls sb_ngx_present_xfb EVERY frame → g_efb_color populated every
  frame → the reflection feedback bootstraps bright from boot → sky should render. So the black sky is
  STRONGLY EVIDENCED as a HEADLESS-CAPTURE ARTIFACT, not a real headed bug.

## CONCLUSION + next step
Headless ngx capture CANNOT faithfully render the airstrip's EFB-copy feedback effects (ocean reflection)
because the per-frame present+readback doesn't run headless. To VERIFY the airstrip's true ngx fidelity
(verify-first / fix-the-harness rule), the harness fix is: **drive the ngx present render every frame in
headless** (maintain g_efb_color) so EFB-copy effects render like headed. THEN re-evaluate the sky.
GXDrawSphere does NOT fire in the airstrip (sky is J3D ti=8/9/11, not the sphere dome). The file-select
dark-windows is the separate residual wash. The missing HUD counter is a separate gap (not investigated).

Tooling added: /pixbatch -901 prints per-texmap wrap/filter/mip; [efb-wb] copytex_writeback return-reason
debug (SUNBRIGHT_DBG_EFB); /magmat; freeze-table fix. SUNBRIGHT_STAGE=0 = airstrip repro.

## CORRECTION (same day) — it is NOT just a headless artifact; the EFB readback reads DARK data
Built the harness fix (Present.cpp: drive the ngx present+readback every headless frame). Verified it
works: /ngxpresentlive frames now INCREMENTS in headless (was stuck 0), the 80f94fe0 EFB copy is now
SERVED every frame, plaza renders unchanged. BUT the airstrip sky is STILL BLACK with the fix → so it is
NOT merely the headless on-demand-present artifact. Since headless now behaves like headed and it's still
black, the black sky is REAL HEADED TOO.

New root (narrowed, the real bug): the served EFB copy reads `center src=000a13` (near-black) CONSISTENTLY
— but the actual rendered screenshot (same ngx render) shows a BRIGHT white platform at screen center.
So ngx's EFB COLOR READBACK (g_efb_color, filled in ngx_present.cpp's render fn; consumed by
sb_ngx_efb_copy_region) reads DARK/WRONG data that does NOT match the rendered frame. The ocean reflection
(ti=42) then samples this dark copy → blends black over the (correct) blue sky base → black sky.
NEXT (fresh investigation): in ngx_present.cpp, find WHICH target g_efb_color is read from and WHEN, vs
the target the screenshot/present writes. They disagree (dark vs bright) → the readback samples the wrong
image or the wrong time (e.g. a cleared/pre-geometry target, or before tone-map/composite, or a layout/
format mismatch). Fix so g_efb_color == the rendered scene; then the reflection samples bright → sky
correct. The headless-present-driver fix is committed and is a prerequisite (EFB effects now verifiable
headless). The ti=42 alpha (a0.72) is the blend weight; the darkness is the copy CONTENT, not the alpha.
