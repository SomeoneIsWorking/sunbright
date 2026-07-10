# 2026-07-10 — Title screen PIXEL-level diagnosis (native vs retail)

Grounded the title-parity work in actual pixels (prior work was structural draw-counts).
Native captured at 640×480 window (1280×960 internal, 2.5×) via `SB_DUMP_FRAME` +
`SB_DUMP_FRAME_AFTER=<presents>`, converted RGBA→PNG. Reference = the settled PRESS START
title `scratch/oracle/frames/check_3800.png` (NOT `title_gx_oracle.png`, which is the LATER
file-select screen — an easy wrong-reference trap).

## State check first (important)
- Native SB_STAGE=15 shows the **PRESS START title logo** (Super Mario Sunshine over sky),
  the correct state — NOT the file-select. `title_gx_oracle.png` (file-select) is a later
  state reached only after pressing START; don't diff against it for the title.
- Native frame 120 and 1200 are both the (broken) title; **frame 3000 is BLACK** — by then
  the attract loop (~45 s idle, CardLoad.cpp) has advanced to a THP attract movie native
  can't decode. So the title window is ~frame 300–2700; sample there, not later.

## The defect: blurry, oversized, blue-white-washed title
Retail: crisp "SUPER MARIO SUNSHINE" logo (~60% of frame) over a light-blue sky with white
clouds + a bright localized sun-flare, palm tree, rainbow trail, ©2002 NINTENDO.
Native (frame 1200): the SAME elements are present (SM letters, SUNSHINE, rainbow, ©2002
NINTENDO all identifiable) but **heavily blurred, the logo blown up to fill the screen, and
the whole frame washed to blue-white.**

Pixel stats (mean RGB / fully-white fraction):
- NATIVE  [210, 210, 254.7]  19.6% fully white  — **blue channel saturated everywhere.**
- ORACLE  [143.7, 177.2, 201.1]  7.8% white.
- Per-frame, NOT accumulation: frame 120 is already washed [190, 217, 241].

## Hypotheses FALSIFIED (do not re-chase for the visible defect)
- **Phase-1 ghost pass**: `SB_SKIP_GHOST=1` drops it (293→166 draws, ortho 125→32) →
  final image **BIT-IDENTICAL**. The ghost draws the 3D scene under stale ortho, which maps
  it offscreen/inert — it is wasted work but contributes ZERO pixels. (Added SB_SKIP_GHOST
  probe in MarDirectorDirect.cpp; unk40 holds only `drawBufferGroup,8`.) So the ghost is a
  perf/structural wart, NOT the parity blocker — stop treating "delete the ghost" as the fix.
- **EFB-copy-sampling quads**: `SB_SKIP_COPY_QUAD=1` → identical. Not the compositor quads.
- **Lens flare**: `SB_SKIP_MARK=LensFlare` → identical. Not the sun-flare/bloom.
- **Accumulation/bad-clear**: early frame already washed → per-frame, not cross-frame.

## Partial contributors CONFIRMED
- **Sky**: `SB_SKIP_MARK=Sky` drops the blue wash noticeably (B 254.7→222.7, R/G 210→170),
  but the white blobs + blurry oversized logo REMAIN. So the sky is over-bright/too-blue but
  is only part of it.
- **Screen texture is HALF-res** (`new JUTTexture(SMSGetGameRenderWidth()/2, …)`,
  ScreenUtil.cpp:218) — 320×240. Retail uses this too, so half-res alone isn't the extreme
  blur, but a broken composite that upscales it over the whole frame would be.

## NDC-probe finding: the 3D scene is MIS-FRAMED (maps outside NDC)

`SB_NDC_PROBE` at the title (min-verts 25–50, perspective + ortho): essentially NO 3D
draw lands inside the visible NDC box. The 202v sky sphere (scale-100000 `GXDrawSphere`)
maps to ndcX ±343..754 (drifting — the map==15 sky spin), `inXY=0`, both under ortho AND
perspective — invisible by geometry (retail draws the same 202v dome `cU=0`, also invisible,
so consistent). The largest in-view thing is a 42v `DrawBuf MapOpa` under ORTHO with a single
vertex barely inside (ndcX up to 138). So native's title 3D backdrop is projected almost
entirely OUTSIDE the frame; the visible blurry blue-white is the 2D/composite layer with no
correct 3D behind it.

Native sky posmtx `M=[-0.595 0 -0.804 -335.1 | 0.650 0.588 -0.481 -132.2 | 0.473 -0.809
-0.350 -96.1]` (a rotation + translation) where **retail's 202v sky posmtx is IDENTITY**.
The scene is drawn camera-relative, so this posmtx IS (view · model); a wrong view matrix
mis-frames the whole 3D scene. This points at the **title camera / view matrix (camera1 +
"J3D System Set View Mtx")**, NOT blend/bloom/fog. This is the same camera-relative math in
`TSky::perform` (the `// TODO: match this awfulness` hand-transcribed inverse-camera formula)
— re-examine it and the camera1 view matrix that feeds the title world pass.

## ROOT CAUSE (confirmed): native color-draws the title; retail draws Z-only + composites

Added blend/cU to the draw dump (aurora: `bm sf df cU aU`). Native settled title = 293 draws,
**~290 are cU=1** (color-writing). Retail settled title = 1258 draws, **only 71 cU=1** — the
entire 3D world+mirror is drawn `color_update=0` (Z-only) and the visible image is composited
from the EFB snapshot (the 26× 52-vert block after EFB copy #2). Retail's dome/sky raster:
`blend_enable=1, color_update=0`. Native's `DrawBuf Sky Xlu`: `cU=1, sf=1(ONE)/df=3(INVSRCCLR)`
— it PAINTS the sky (bright blue) where retail paints nothing, which is the blue-white wash.

So the camera is fine (framing ≈ correct; the up-at-sky camera settles cleanly — pos
(1095,328,-13) → target (532,1136,158), fovy 40 = retail's proj), blend is mostly normal,
additive draws are offscreen (LensFlare/Sky-sphere, proven inert). The defect is
**architectural**: native lacks retail's Z-only-then-EFB-snapshot-composite path for the title
and instead directly color-draws every pass. The blown-out/blurry result is that direct render
(over-bright sky blend + the 256×256 mirror pass + no snapshot composite) instead of retail's
composited frame.

## CORRECTION (verified): native DOES do the EFB copies — 2/frame

Earlier "native does 0 GXCopyTex" was WRONG — read off a STALE binary after a silently-failed
build (an `sb_gx_last_marker` scope error). With `SB_COPY_DBG` + `SB_EFBTEX_DBG` on a clean
build, native does **4476 tex-copies over the run (~2/frame)**: `鏡描画ステージ` (mirror,
256×256) and `通常シーン描画ステージ` (normal-scene snapshot, **320×224 = half-res**), both
firing `TEfbCtrlTex::perform` with the draw bit 0x8 and a valid `mImagePtr`. So the EFB-copy
pipeline is NOT missing. (LESSON: always confirm BUILD=0 before trusting a run; a failed build
leaves the prior binary and its instrumentation silently absent.) The blur is therefore the
**half-res (320×224) normal-scene snapshot in the composite path** (2× upscale to 640×448),
not an absent copy. Next: find whether `合成3` / the snapshot composite is drawn full-screen
over the frame (it shouldn't dominate the crisp direct render — retail's title is crisp despite
the same half-res snapshot object existing). NOTE `SB_SKIP_COPY_QUAD` did NOT change the image,
so either the composite isn't flagged as a copy-sampler or it isn't the visible layer — resolve
that before assuming the composite is the blur.

**The (revised) fix direction = the title color-update + snapshot-composite semantics** (native
color-draws cU=1 where retail is Z-only+composite; and the half-res snapshot path):
retail's `TEfbCtrl`/`TEfbCtrlTex` (`JDREfbCtrl.cpp`) set `GXSetColorUpdate(false)` for the 3D
passes and drive the mid-scene EFB copy; the 26-draw compositor block then paints the visible
backdrop by sampling that snapshot. Native's EfbCtrl objects aren't managing cU / aren't doing
the mid-scene copy, so everything falls through as a direct cU=1 draw. This is a subsystem port,
not a one-line fix. Diagnostics wired for it: SB_DRAW_DUMP_FRAME now prints vp + bm/sf/df/cU/aU;
SB_SKIP_GHOST, SB_CAM_DBG, SB_NDC_PROBE. NOTE: SB_SKIP_ORTHO and SB_SKIP_MARK=Mirror both HANG
the boot (fades/mirror-copy are load-bearing) — don't use them.

## DEEPEST ROOT (traced): the title SCENE is under-populated

Native draws ~10× fewer draws than retail (293 vs 1258; mirror 71 vs 653; the AfterIndirect
reflective-sea composite is **1 draw vs retail's 26× 52-vert block**). Traced via `SB_SEA_DBG`
(now `[sea-perform]` in `TMapStaticObj::perform`): at the settled title (camera confirmed
settled via `SB_CAM_DBG`), the ONLY `TMapStaticObj` performing is **`sun_mirror`**
(太陽in鏡, from `Camera/sunmodel.cpp:112`), param_1=0x6 (calc only), never the entry pass
(0x200), UNK80=0. Retail's title has the reflective sea + scenery as many static objects.

The `initStageCommon` sea (`波（遠景）`/`インダイレクト波`, Map.cpp:146-150) is gated on
`getCurrentMap() ∈ {4,3,0xD,9,5,6,0x14,0,1}` and the **title map is 15** (TSky `mMap==15`),
so it's correctly excluded — the title's scene objects come from the title **scene.bin**, and
native only instantiates `sun_mirror` from it. So the title defect bottoms out at
**scene-loader / object-creation completeness for the title stage**: native creates a tiny
fraction of the title's scene objects, so most passes (mirror reflection, the reflective-sea
composite that paints the visible backdrop, scenery) have nothing to draw → the sparse,
over-bright, blurry result.

## Engineering assessment (title parity scope)
Title parity is NOT a single bug — it is a stack: (1) the title scene.bin objects mostly
aren't created (only `sun_mirror` seen), so passes are near-empty; (2) with no composited
backdrop, native falls back to direct cU=1 color-draws of the sparse content (over-bright sky
blend); (3) the half-res (320×224) snapshot path. The EFB-copy plumbing itself WORKS (2
copies/frame). The dominant fix is (1): get the title stage's scene objects created. That is a
scene-loader / stage-setup port, related to the gated `TMapObjTree::initMapObj` work, not a
render tweak. Diagnostics all committed (`SB_SEA_DBG`, `SB_CAM_DBG`, `SB_COPY_DBG` incl.
tex-copies, `SB_EFBTEX_DBG`, blend/cU + vp draw dump, `SB_SKIP_GHOST`).

## Per-draw diff + a foundational doubt (2026-07-10, session end)

Mechanical per-draw diff native-vs-retail (both nominally the PUSH START title):
- Retail world pass (SEG1): 4v×228, 5v×111, 3v×45, … Native world pass: 4v×30, 5v×7, 3v×0.
- Retail has ~472 4v PERSPECTIVE draws at posmtx translation **(0,0,0)** (camera-relative),
  split ~245 (mirror SEG0) + ~228 (world SEG1) — i.e. one big camera-relative object drawn
  as hundreds of small strips (nverts at origin: 4v×343, 5v×145, 3v×131, 6v×58, 8v×40 …).
  Native draws ~30 of these.
- Retail mirror pass = 653 draws vs native 71.

**But `sky.bmd` has only 4 shapes / 4 materials** — so those ~900 small camera-relative
strips are NOT the sky model. Native cannot produce hundreds of sky/cloud draws from a
4-shape sky. This exposes a foundational doubt this session never resolved: **is retail's
`title_press_start_vi_stable.dff` the SAME game state as native `SB_STAGE=15`?** They both
show the logo, but retail's scene is far richer with content native has no source for. If the
states differ (e.g. retail's attract-title map/scene ≠ native's map-15 "Option"), the entire
293-vs-1258 "sparse scene" comparison is confounded, and the real defect may be ONLY the
uniform over-bright/blur, not missing objects.

**What's needed to break the deadlock (not done this session):** a LIVE Dolphin capture at
native's EXACT state (same map/stage/tick), or confirmation of what map/scene retail's title
FIFO actually is. Without a matched oracle, per-draw diffs mislead. ~10+ render/scene
hypotheses were each falsified (see above); the state-match question is the missing
foundation. STATUS: title parity UNRESOLVED; needs a matched-state oracle before more work.

## Superseded hypotheses (this session, do not re-chase)
The logo/scene appears **oversized** (SM letters fill the screen vs ~60% in retail) +
blurry. That reads as a SCALE/PROJECTION divergence in the title's perspective pass (retail
world proj diag = [2.04163, 2.74748]) OR a broken screen-texture composite stretching a
half-res capture over the frame. Next: capture native's actual title perspective projection
+ the world-camera matrix and diff against retail's; and dump the intermediate screen-texture
/ EFB-copy contents to see what the composite samples. `SB_SKIP_ORTHO` HANGS the boot (fade
screens are ortho — a load loop waits on a fade that never renders), so isolate 3D-vs-2D some
other way. Do NOT diff against `title_gx_oracle.png`; use `frames/check_3800.png`.

## ★★★★★ RESOLVED PREMISE: the "293 vs 1258 sparse scene" is a DRAW-MERGING ARTIFACT

The entire multi-attempt "native title scene is 10x sparser than retail" premise was WRONG.
Aurora MERGES consecutive same-state draw calls (command_processor.cpp:2170-2208, canMerge +
`lastDraw->vertRange.size += vertRange.size`); Dolphin's .dff FIFO records every emitted draw
UNMERGED. Proof: `TMapObjWave::draw()` (the reflective sea) emits **26 triangle strips**
(`SB_WAVE_DBG`: `strips=26 vpp=52`) but the aurora draw dump shows **1** 52-vert draw — the 26
merged into 1. So native's 293 merged draws correspond to retail's 1258 unmerged. **Native
renders the SAME content** — the sea (26 strips), clouds, objects are all there; they're just
merged in the dump. The camera is also correct: native's view-matrix translation
(306,-1043,-354) matches retail's wave posmtx (305,-1043,-353) to <1 unit.

**Do NOT re-investigate "missing objects / sparse scene / missing sea / missing clouds."** It
was a capture-method artifact. The ONLY real title defect is the **uniform over-bright (blue
saturated) + blur** of the correctly-framed, correctly-populated content — a post/compositing
or global-state bug, NOT missing geometry. Focus there: the half-res (320x224) normal-scene
snapshot composite (`通常シーン描画ステージ` + `合成3`), and any global brightness/gamma in the
present path. Every draw-count-based comparison this session is void; use PIXELS + per-draw
state, never draw counts, for aurora-vs-Dolphin.

## Loop iteration (2026-07-11): visible defect is J2D huge-tiled pictures

`SB_J2D_DRAW_DUMP` (J2DPicture::drawFullSet) shows the attract title's J2D pictures drawn at
ENORMOUS bounds (w/h 2000–10107, up to h=14438) with tiny 40–63px 2-tone textures (mBlack blue
0000ff00, mWhite ffffffff) in wrap mode — that tiled blue↔white fill IS the blurry blue-white
wash. Bounds GROW over the run (40→384→…). Ruled OUT: `TCoord2D`/`CLBChaseGeneralConstantSpecifySpeed`
overshoot (it clamps at target correctly), so the interpolator isn't the unbounded-growth source.
J2DPane reads .blo bounds as fixed S16, so the size comes from a large resize TARGET or a large
.blo pane. drawFullSet computes tiling UVs (renderWidth/texW ≈ 50×) — blur = if the texture wraps
CLAMP not REPEAT, those UVs stretch the edge texel instead of tiling.

OPEN for next iteration: (1) are these huge panes the SETTLED title or intro-transient? (frame-gate
the dump). (2) Which .blo is being drawn — `title_1.blo` (unk34) or the loading screen
(load.blo/unk28)? CardLoad::draw switches screen by state; a wrong state would draw the wrong .blo.
(3) texture wrap REPEAT-vs-CLAMP for the tiled pictures. Localized to the J2D 2D layer — NOT 3D,
camera, or content (those are correct; sparse-scene was a merge artifact).

---

## 2026-07-11 — SPIRV harness fix + bounds premise DEBUNKED + scene-is-present reframe

Major corrections to this doc's earlier leads:

1. **RENDER HARNESS WAS BROKEN (now fixed).** The title crashed 5-6/6 on cold pipeline
   compile with `Produced invalid SPIRV` — root cause was aurora's WGSL vertex-fetch
   helpers using **dynamic-offset `extractBits`**, which Tint miscompiles (clamped lowering
   → OpExtInst(GLSL.std.450) invalid operand). Fixed in aurora (`9dcd5e2`, shift+mask). The
   2026-07-10_title_fidelity_infra.md "closed as ccache staleness" verdict was WRONG and is
   corrected there. ALL prior captures were with a warm dawn_cache masking this. Any capture
   requires `SUNBRIGHT_ROM` set (else dvd_open fails → misleading "crash").

2. **The "J2D pictures draw at huge 2202px bounds" premise was a STALE-BINARY ARTIFACT.**
   On a clean build the `[j2d-fullset]` dump shows NORMAL bounds (mBoundsW≈160/121/140,
   clamped to texture size ≈74px, centered). There is NO bounds/scale bug. Do not re-chase it.

3. **Logo-letter textures decode CORRECTLY.** tex_382-385 are I8/IA4 (fmt1/fmt2) duotone
   glyphs; dumped RGBA shows R==G==B grayscale with alpha==intensity (corr 1.0). getTransparency
   returns 2 (truthy) → TEV uses GX_CA_TEXA. mBlack=0x0000ff00 (blue) is the INTENDED duotone
   (blue letters, matches oracle). Duotone stage alpha = 255*TEXA → background alpha 0 →
   should be transparent. TEV/blend (bm=1 sf=4 df=5 = GX_BM_BLEND SRCALPHA/INVSRCALPHA) correct.
   The letters have a LARGE soft glow halo filling most of the texture rect.

4. **The 3D scene IS present in native.** frame-140 draw-dump: 291 draws = 134 PERSPECTIVE +
   157 ortho, marks include DrawBuf Mirror Opa ×142, MapOpa, Sky Xlu, LensFlare,
   TLightDrawBuffer::Opa, ShadowOpa, MapXlu. So geometry is NOT wholesale missing.

5. **Oracle settled title is predominantly 3D PERSPECTIVE** (title_press_start_vi_stable.dff:
   1258 draws, ORTHO only appears seq>=21074 as a late-frame tail). The logo/sun/palm/rainbow
   are 3D models, not 2D pictures.

**OPEN (next):** captured image shows sky+clouds + washed 2D glow-letters but LACKS the crisp
3D logo / palm / rainbow / ocean-reflection that the oracle shows — even though native emits
perspective Map/Mirror/Sky draws. Next step: per-scene-element diff (which perspective draws
land on-screen vs off / wrong-colored), NOT more J2D-bounds work. Also re-scrutinize the
"291 vs 1258 = pure merge artifact" claim: a 4.3x gap may partly be genuinely-absent draws
(logo model?), not only aurora draw-merging — validate before trusting.

---

## 2026-07-11 (cont) — "Oversized washed logo" was CAPTURE-TIMING; real defect = logo lacks reflection fill

**★ The "logo renders ~2x oversized + blurry/washed" defect does NOT exist at the settled
title — it was a mid-animation capture.** Captures at `SB_DUMP_FRAME_AFTER=200` land while
the title TExPane logo is still animating in (zooming/forming) → looks huge and washed. At
`SB_DUMP_FRAME_AFTER=300` (settled) the render is STRUCTURALLY CORRECT and close to the
oracle: "SUPER MARIO SUNSHINE" logo at the right size/position, shine sprite, palm tree,
rainbow trail, ©2002 NINTENDO, sun flare — all present and placed correctly (see
scratch/screenshots/t_300.png + sbs_300.png side-by-side vs check_3800). **Always capture the
title at >=300 presents.** The binding=15/wrapH=wrapV=1/texture-stretch analysis was correct
but IRRELEVANT — those are authored .blo values (verified against raw bytes via
SB_J2D_PIC_HEXDUMP: mBinding byte=0x0f, wrapMode byte=0x05) and the oracle stretches the
same way; the stretch is not the defect.

**Real remaining defect (subtle, well-scoped):** the logo letters render WASHED-OUT WHITE /
semi-transparent, MISSING the blue-sky-top / teal-ocean-bottom ENVIRONMENT-REFLECTION interior
fill that the oracle letters have. Oracle "SUPER MARIO" = solid blue with 3D shading + the sun
emblem red/yellow; mine = pale ghostly white glyphs (correct shape, wrong fill). The I8
letter textures (big_tx_g/o/ex/s.bti) supply only the white glyph + rainbow corner-color tints;
the BLUE/colored fill must come from an additional layer the render is missing — most likely
the EFB-copy environment/reflection snapshot applied to the logo (the named
TEfbCtrlTex->GXCopyTex snapshot arc / AfterIndirect indirect-texture pass; native emits only
1 'AfterIndirect Xlu' draw). NEXT: trace where the oracle's blue logo fill comes from
(separate blue base picture layer vs indirect/EFB reflection) and port it.

Raw pixel diff t_300 vs check_3800 = 62.7 mean but DOMINATED by cloud-pattern misalignment
(top-left quadrant 119.5 — different animation phase of the moving clouds), NOT the logo.
Do not use whole-frame pixel diff as the metric while clouds animate out of phase.

---

## 2026-07-11 (cont) — ★★★ TITLE LOGO RENDERS FAITHFULLY — the "washed logo" was ALWAYS animation phase

**RESOLVED: there is NO title-logo rendering bug.** The logo is a single big base texture
`tex_355` (460x304 RGB5A3) = the COMPLETE finished blue "SUPER MARIO SUNSHINE" logo (shine
sprite, palm, rainbow, red/yellow sun emblem, ocean reflection — identical to the oracle),
drawn at pane bounds 534x353, PLUS individual fly-in glyph layers (I8/IA4, white via duotone)
that animate during assembly.

The logo animation OSCILLATES by present count (attract cycle): captured blue-letter-spot
mean RGB was 300=[234,241,250] (white, mid-assembly), 700=[156,178,236] (settled blue),
1200=[238,238,255] (white again — re-animating / attract restart). At the settled hold
(frame ~700) native matches the oracle closely: see scratch/screenshots/sbs_700.png — blue
3D-shaded logo, rainbow, shine, palm, sun emblem, (c)2002 all correct. Oracle blue-letter
mean [80,131,213] vs native-700 [156,178,236] (slightly lighter, essentially the same).

So the multi-SESSION "logo ~2x oversized / washed white / blue boxes" investigation was
ENTIRELY an artifact of capturing mid-assembly animation phases (glyphs flying in / base not
yet composited), compounded earlier by the SPIRV harness crash + a stale-binary huge-bounds
misread. The J2D picture path, bounds, wrap, binding, duotone, and base-texture composite are
all CORRECT.

**Remaining title diffs (minor, not the logo):**
1. PRESS START prompt at a different fade/blink phase (native-700 shows full faint "PRESS
   START!" arc across top; oracle check_3800 shows just "PR" fading in) — animation phase.
2. Slightly lower logo saturation at frame 700 (may deepen a few frames later).
3. Sky cloud pattern out of phase (moving clouds; different frame) — inherent to non-identical
   frames, not a bug.

**Capture protocol:** the settled blue hold is around ~700 presents but OSCILLATES; to pin a
stable settled frame, sweep SB_DUMP_FRAME_AFTER and pick the frame whose blue-letter-spot
RGB is closest to [80,131,213]. Next parity work = PRESS START prompt phase + whether native
reaches a STABLE hold (oracle holds 3 identical frames) or keeps re-animating.

---

## 2026-07-11 (cont) — REAL DEFECT FOUND: fly-in glyph layers don't fade out → wash blue base

Refines the "faithful modulo phase" claim: there IS a real (subtle) defect. Frame sweep
(sw_650..980) shows the logo blue-spot mean plateaus at ITS BLUEST ~[155,180,237] (frame
650-720) and NEVER reaches oracle [81,133,215] — then lightens further (transition). So it is
NOT pure animation phase.

DECISIVE isolation (SB_SKIP_DUOTONE=1, new diagnostic in J2DPicture::drawSelf — drops
mBlack!=0/mWhite!=white "duotone" pictures = the fly-in glyph/highlight layers): with the
glyph layers removed, the base-only blue-spot = **[81,133,216]**, matching oracle
**[81,133,215]** EXACTLY. And tex_355's own blue-core texels = [89,145,219] ~= oracle. So:
- Base texture tex_355 (the complete finished blue logo) renders oracle-perfect on its own.
- The white fly-in GLYPH layers (duotone I8/IA4 pictures) are drawn ON TOP and dilute the
  blue to [153,177,236]. They persist at the settled hold instead of fading out.
- At the oracle's settled hold only the pure blue base shows → the glyphs ARE supposed to
  fade/hide once the logo assembles; native's glyph fade-out is incomplete.

NEXT: find the title logo assembly→hold transition (CardLoad / TExPane 'titl' animators) and
why the fly-in glyph layers' alpha doesn't reach 0 (or they aren't hidden) at settle. The
tev-name dump shows duotone draws at BOTH mColorAlpha=0 (faded) and =255 (visible) — some
glyphs stuck visible. That is the remaining title-parity fix.

---

## 2026-07-11 (cont) — CONFIRMED: logo saturation is a REAL bug, not phase (fine sweep)

Fine every-40-present sweep over the whole title (149 frames, multiple attract cycles;
SB_DUMP_FRAME_EVERY): among 104 logo-showing frames the bluest logo blue-spot is a very
CONSISTENT [148,176,235] and NEVER approaches oracle [81,133,215]. Steady-state => a real
rendering difference, not animation phase. Base-only (SB_SKIP_DUOTONE) = [81,133,216] =
oracle, so the mBlack=0x0000ff00 IA4 duotone overlay letters render white-ish over the base
and dilute it. Duotone math is correct-per-spec (IA4 body intensity ~226 => lerp to
mWhite=white); aurora IA4 decode matches Dolphin. So the open question is whether the ORACLE
draws these duotone letters at the hold (=> aurora TEV renders them lighter, aurora bug) or
hides them (=> game visibility). Next: oracle FIFO --tev-tsv ground truth.
