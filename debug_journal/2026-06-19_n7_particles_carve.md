# N7 particles (JPA) — the carved path (2026-06-19)

User directive this session: "from top to bottom. PC native. RE and port. carve a path." Worked the
EFB-readback family (top of CLAUDE.md gaps) top-down, hit verifiability walls on the transient
effects, then carved into N7 (particles), the documented next renderer stage. This is the durable
plan + RE for the JPA port. Verification tooling is PROVEN (below) — TOOLING-FIRST satisfied.

## Why the EFB-readback family is exhausted for cheap-verifiable work (don't re-walk)
- **Sun occlusion (GXPeekZ)**: UNREACHABLE in plaza — sun at ~60° elevation, camera tilt saturates
  with sun stuck at NDC-Y≈6.9, all 17 sample points (-1,-1), GXPeekZ fires 0×. It's the Noki Bay
  sun-warp effect (flag 0x50004, map 1). Port exists (dormant). See
  2026-06-19_sun_occlusion_unreachable_in_plaza.md (committed 04ee71f).
- **dash-blur (TAfterEffect::perform 0x8022d4f8)**: a real gap (ngx drops immediate-mode GXBegin
  screen quads — only GXDrawCube/Sphere are special-cased), but NOT triggerable in free-roam plaza:
  gpAfterEffect=*(0x8040E0B8); across spin-jump/dive/run/crouch, flags14 bit2 (blur-active) never
  sets, unk50 stuck at floor 0.02 (needs the turbo-nozzle/secret-stage dash speed). When inactive it
  draws a transparent quad (invisible). Park until a triggering scene exists.
- **texgen-matrix faithfulness**: CONFIRMED resolved (in-code comment ngx_j3d_shape.cpp:570-583 =
  xfmem-lag trap; mTotalMtx@+0x64 authoritative). ab_oracle freeroam = 17.0% = baseline, no gap.
- Done+verified already: GXCopyTex water/mirror, GXPeekARGB Mario occlusion.

## The N7 particle gap (CONFIRMED real + reachable + verifiable)
- ngx renders from the J3D object model + special-cased GXDrawCube/Sphere. **No override captures the
  JPA draw path** → all JPA particles (FLUDD spray, ambient effects, NPC effects) are DROPPED under
  ngx present. This is N7 (docs/native_port_plan.md "N7 — Particles (JPA)").
- Reachable: FLUDD spray in free-roam plaza. Continuously present (unlike the transient EFB effects).
- **Verification PROVEN deterministic**: `scratch/spray_plaza.sav` (made via /pad do=r + /savestate
  mid-spray). ab_oracle spray_plaza = 21.3% (vs 17.0% static baseline). ngx-vs-ngx CROSS-RUN drift on
  the spray save = **0.6%** (= static-plaza determinism) → the save is deterministic, ab_oracle's
  number is real signal NOT drift. ⇒ Valid verification = compare ngx spray_plaza BEFORE vs AFTER the
  port (same save, same camera, only my code changes); the GX oracle (abo_oracle.ppm) is the target.
  The whole-frame number is noisy (parked wash + HUD + NPCs dominate); use the SPRAY-REGION before/
  after delta + visual (the plume must appear in ngx where it's currently absent).
  NOTE: make a more dramatic FORWARD-spray save (Mario facing camera spraying out) for a clearer
  plume than the current ground-ring save.

## RE of the JPA draw path (the port target)
- **Seam**: `JPAEmitterManager::draw(JPADrawInfo*)` @ **0x80324f58** → drawBase ×8 groups → per
  emitter `mDraw.draw(info->getCameraMtxPtr())` (JPADrawVisitor). Override it under ngx present:
  run original (Dolphin state), then walk emitters/particles and emit ngx billboards. (Per-particle
  GXBegin can't be tapped — GP-FIFO parse is rejected; synthesize from the object model like
  imm_geom_native.cpp's GXDrawCube/Sphere.)
- **Per-particle billboard** (JPADrawVisitor.cpp ~L341+): `GXBegin(GX_QUADS,4)`, 4 corners =
  `offs[i] + pt`. Two classes: screen-aligned (offs.z=0, pt.z const → camera-facing) and
  world-oriented (offs.z used). offs come from particle scale × camera basis. TEV colors set via
  GXSetTevColor(TEVREG0=prm, TEVREG1=env) per emitter (JPADrawVisitor.cpp L204-267).
- **Data layouts** (reference/sms/include/JSystem/JParticle/):
  - JPABaseEmitter: particle list `mParticleList` (JSUList) via getParticleList(); mScale; the draw
    object `mDraw`; the shape/resource for texture+blend. (Get exact offsets from JPAEmitter.hpp +
    disasm of JPAEmitterManager::drawBase / mDraw.draw.)
  - JPABaseParticle (JPAParticle.hpp): mGlobalPosition @ +0x2C (TVec3 f32), mLocalPosition @ +0x20,
    mVelocity @ +0x38, mAge +0x44, mLifeProgress +0x48, mLifetime +0x4C, mBaseVelocity +0x5C. The
    render-calc sub-struct: mAlpha @ +0x20, scale fields unkC/10/14, mPrmColor @ +0x2C, mEnvColor
    @ +0x30 (VERIFY these offsets against the real class split via disasm before trusting).
  - Texture: emitter shape → texture index → JPA resource (JUTTexture) → decode (reuse N1
    sb_tex_decode) → ngx texture; blend mode + alpha-test + zmode from JPABaseShape.

## Implementation plan (thin vertical slice first, each verifiable)
1. Override JPAEmitterManager::draw (0x80324f58), s_ngx_present-gated, run-original-around. DBG: count
   live emitters + particles in spray_plaza (validate the seam fires + data offsets). Commit.
2. Emit FLAT-colored camera-facing billboards (no texture) into ngx batch (reuse imm_geom emit path
   ngx_emit_imm + NgxPEState) at each particle's mGlobalPosition, size from scale, prmColor. Verify
   colored dots appear in ngx spray_plaza where particles are. render_test unit for billboard offs
   math (camera-basis × scale → 4 corners). Commit.
3. Add the particle TEXTURE (decode from JPA resource, bind), texcoords, and the shape's BLEND mode
   (additive/alpha) + TEV (prm/env). Verify spray-region ngx≈GX. Commit.
4. Iterate: billboard-type variants, color/alpha animation, ext/extra shapes, the long tail.

## Recipes (this session, verified)
- gpAfterEffect=*(0x8040E0B8); gpSunModel via *(r13-0x70F8), r13=0x804141c0.
- Build build-freshtest; ab_oracle: SUNBRIGHT_BIN="$PWD/build-freshtest/sunbright"
  tools/render/ab_oracle.sh scratch/spray_plaza.sav 6. render_test: ./build-freshtest/sunbright-render-test.
- ngx emit pattern to copy: runtime/overrides/imm_geom_native.cpp (GXDrawCube/Sphere → ngx batch).

---

## UPDATE (2026-06-19 pm) — steps 1+2 DONE & committed (d975f1a). Read this before step 3.

Implemented the JPA particle capture. Files: `runtime/overrides/jpa_particle_native.cpp`,
`runtime/ngx/ngx_jpa_billboard.h` (pure billboard corner math + render_test `jpa_billboard`),
`ngx_emit_particle_quad_eye` in `runtime/overrides/ngx_j3d_shape.cpp`. render_test 14/14.

### Seam + offsets (ALL verified via disasm — don't re-verify)
- Seam = **JPADraw::drawParticle @ 0x8032bd10** (NOT setParticleClipBoard, NOT the manager). Per
  emitter; run original first (populates cb + per-shape GXSetZMode/GXSetBlendMode + draws to dropped
  EFB), then walk + emit. drawChild() runs only AFTER drawParticle returns ⇒ cb/z/blend are the
  PARENT's. emitter = gpr3(JPADraw*) − 0x30.
- **JPADraw::cb = 0x8040C110** (static JPADrawClipBoard). unk4.x@+0x14, unk4.y@+0x18, unkC.x@+0x1C,
  unkC.y@+0x20, mViewMtx@+0x34 (MtxPtr), mPrmColor@+0x98 (GXColor RGBA bytes).
- JPABaseEmitter: mDraw@+0x30, mParticleList(JSUList mHead)@+0xF4. JSULink: mData@+0, mNext@+0xC
  (link addr == particle addr, both at particle+0). JPABaseParticle: flags@+0x10 (INVISIBLE 0x8),
  mGlobalPosition@+0x2C. JPAParticle.mDrawParams@+0xA0: scaleX(unk10)@+0xB0, scaleY(unk14)@+0xB4,
  mAlpha@+0xC0, mPrmColor@+0xCC.
- Billboard (JPADrawExecBillBoard, exec @ 0x8033025c): pt = mViewMtx·globalPos (eye space), corners
  add screen-aligned half-extents in eye X/Y at constant eye Z. Half: x1=sx·(u4x−ucx), x0=sx·(u4x+ucx),
  y0=sy·(u4y+ucy), y1=sy·(u4y−ucy); {(−x0,y0),(x1,y0),(x1,−y1),(−x0,−y1)}.
- GXSetZMode @ 0x80361f54 (r3=enable,r4=func,r5=update), GXSetBlendMode @ 0x80361dd0
  (r3=type,r4=src,r5=dst). Tapped live. Plaza spray shapes: z(test=1, func=LEQUAL, write=0),
  blend (BLEND, SRCALPHA, INVSRCALPHA) or (BLEND, SRCALPHA, ONE). zmode is FAITHFUL, not hardcoded.

### What's verified
- Geometry/transform/colour CORRECT: SUNBRIGHT_JPA_SHOW (opaque magenta, z-off) renders the spray
  cloud exactly where it belongs. eye.z matches the J3D scene eye-space (same projection + values).
- With the real shape zmode the particles render clustered at Mario + the spray impact; the rest are
  correctly depth-occluded. On scratch/spray_fwd.sav (forward-spray) particles move ngx TOWARD the
  GX oracle (51.45→51.22 full-frame, 17123 px changed).

### Depth gotcha (understood, NOT a bug to chase)
The projection z-row at particle time is [0,0,-0.00003,-10] (near≈10, far≈huge) → depths squish to
vulkanZ≈0.98-0.99; the mesh.vert shader does z+w (GC ndc [-1(near),0(far)] → Vk [0,1]). Scene AND
particles share this projection, so depth IS consistent. SUNBRIGHT_JPA_ZFUNC=7(ALWAYS)/6(GEQUAL) made
the SHOW cloud appear, LEQUAL(3) occluded it — i.e. the SHOW cloud is genuinely mostly BEHIND the
scene in the ground-ring save (weak spray). With the forward-spray save it renders. NOTE: cb.mViewMtx
varies per emitter/manager (some use a distant camera, Z-trans ~-13581 → eye.z ~-15k, tiny/off-screen);
that's REAL (multiple JPA cameras), not a bug — gameplay-camera emitters place correctly.

## UPDATE (2026-06-19 late) — STEP 3 DONE & verified (particle TEXTURE + full TEV)

Implemented the particle texture + the shape's real TEV combiner + the 4 quad texcoords. The
emit (`ngx_emit_particle_quad`, ngx_j3d_shape.cpp) now takes a `ngx_jpa::NgxParticleQuad` (shared
header) carrying eye corners + uv[4] + color_env/alpha_env + C0(prm)/C1(env) TEV registers + the
texmap0 binding + live blend/zmode. render_test `jpa_billboard` now also pins the combiner encoding
(jpa_color_env/jpa_alpha_env vs the shader's decode_cc/decode_ac) — 14/14 PASS.

### Object-model chain (ALL disasm/dump-verified live)
- **Texture**: `JPADraw.mDrawCtx` @ jpadraw+0x90; mBaseShape @ +0x94, mTexResource @ +0xAC.
  texid = getMainTextureID(0) (disasm 8032c700): texanim(unk80@baseShape+0x80) → table[0]
  (mTextureIndices u8* @ baseShape+0x08), else baseShape.mTextureIndex @ +0x7F.
  **JPATextureResource.unk2C @ +0x2C is a `JPATexture**` POINTER — deref FIRST, then index**
  (the bug that gave 17792x32922/fmt=128 garbage was indexing texRes+0x2C directly). jtex =
  *(table + texid*4); ResTIMG = *(jtex + 0x8 /*JUTTexture*/ + 0x20 /*mTexInfo*/). Decode the
  ResTIMG like capture_textures (fmt@0,w@2,h@4,wrap@6/7,filter@0x14/15,tlutfmt@9,imgOff@0x1C,
  palOff@0xC). Plaza spray = one shared 64×64 IA8 (fmt 3), texcoords (0,0)(1,0)(1,1)(0,1).
- **Combiner**: JPABaseShape.TevArgs @ baseShape+0x48 (4 × GXTevColorArg s32 = the GX_CC_* a/b/c/d
  baked from data[0x30] colour-input type). Alpha combiner is fixed TEXA·A0. C0 = prm
  (RegisterPrmColorAnm, per-particle), C1 = env (per-particle), computed via JPA_U8_THRE.
- **Blend/zmode**: live from the GXSetBlendMode/GXSetZMode tees (blend is set per-shape in
  JPADraw::draw before drawParticle; the normal path sets GXSetAlphaCompare(ALWAYS) → NO alpha
  test, so none is applied — faithful).

### Verified (the texture pipeline WORKS)
- `SUNBRIGHT_JPA_TEXSHOW=1` (textured, opaque, out=TEXC) + `JPA_ZFUNC=7` shows the SPRAY TEXTURE
  applied (soft water streaks, vs flat magenta under JPA_SHOW) → texture decode + UV + sampling all
  correct. The texture chain resolves to a valid 64×64 IA8 (DBG_JPA confirms addr/fmt/w/h sane).
- **DRIFT-FREE A/B (new tool `/ngxnojpa?v=`)**: one process, capture WITH then toggle JPA off and
  capture WITHOUT → 2.6% delta, a particle cluster AT MARIO (the FLUDD spray location) in the diff,
  plus expected HUD-trail drift at the counters. So the textured particles RENDER, visible, in the
  normal LEQUAL view (not fully occluded). (Two-PROCESS WITH/WITHOUT is unreliable here — the HUD
  counter motion-trails drift between processes and swamp the small particle delta; use /ngxnojpa.)
- Whole-frame vs GX (spray_fwd) ≈ 51 both with/without ⇒ INCONCLUSIVE: the parked Delfino wash +
  low particle alpha + partial depth-occlusion dominate; particles blend over an already-wrong
  (washed) ngx background so a whole-frame number can't isolate them (the multi-layer no-oracle
  trap). Verdict rests on TEXSHOW (texture renders) + the drift-free cluster-at-Mario.

### Diagnostics added (kept)
- `SUNBRIGHT_JPA_TEXSHOW=1` = textured + opaque (no blend) — isolates texture from blend/alpha.
- `/ngxnojpa?v=1` (sb_jpa_set_disable) = runtime JPA-emission toggle for the drift-free A/B.
- DBG_JPA now prints tex addr/w/h/fmt, the cc(a/b/c/d), the per-particle al (mAlpha) + resolved A0.

### Step 4 backlog (unchanged): billboard-type variants (Y/dir/rot/stripe — unk34[]/unk70[]),
per-particle texanim (unk3A), colour/alpha animation, ext/extra shapes, child particles. Also: the
spray reads SUBTLE under LEQUAL (low alpha + depth) — if a more-visible plume is wanted, revisit
the depth-occlusion (handoff's "depth gotcha"; was declared understood/not-a-bug, but a clearer
forward-spray save or a depth re-check may be the real next visual win).

## UPDATE (2026-06-19 later) — STEP 4 partial: billboard-TYPE variants (Directional t3 + DirBillBoard t9)

Reachability mapped via a type histogram (DBG_JPA `[jpa-types]`): plaza spray uses 4 draw types —
**t2 BillBoard (~1574, was the only one handled), t3 Directional (~188), t6 StripeCross (~549),
t9 DirBillBoard (~90)**. The non-t2 types were being mis-rendered as screen-aligned billboards.

Ported t3 + t9 (the per-particle oriented quads; share my eye-space emit). Type = `mBaseShape->getType()`
@ baseShape+0x69 (selector in setDrawExecVisitorsAfterCB: 0 Point,1 Line,2 BillBoard,3 Directional,
4 DirCross,5 Stripe,6 StripeCross,7 Rotation,8 RotCross,9 DirBillBoard,10 YBillBoard).
- **DirBillBoard (t9)** — eye-space: dir × cameraUp(mViewMtx col1), normalize, rotate into eye (mViewMtx
  3×3) → (ex,ey); the 2D offsets are rotated by the complex factor (ex+i·ey). Same eye pt as billboard.
- **Directional (t3)** — WORLD-space: Gram-Schmidt basis [axis|dir|side] from params.unk0 (axis) and
  the dir vector; local offsets rotated to world, + world pos, then mViewMtx → eye per corner.
- **dir vector (mDirType @ baseShape+0x6A)**: 0=mVelocity(+0x38), 1=mLocalPosition(+0x20), 2=−localpos.
  Types 3 (emitter dir) / 4 (prev particle) → fall back to billboard (uncommon; not yet ported).
- Particle offsets: mVelocity@+0x38, mLocalPosition@+0x20, mGlobalPosition@+0x2C, FLAG@+0x10 (INVIS 0x8),
  drawParams@+0xA0 (unk0/axis@+0, scaleX@+0x10, scaleY@+0x14, mAlpha@+0x20, prm@+0x2C, env@+0x30).
- IMPORTANT: the original guest drawParticle (run first under the override) already orthonormalises &
  writes back params.unk0, so we READ the settled axis — no write-back from the port.

Pure math (jpa_dir_basis / jpa_directional_corners / jpa_dirbb_offsets) is in ngx_jpa_billboard.h and
**render_test-verified** (basis orthonormality, degeneracy rejection, corner + 2D-rotation cases) —
14/14. Integration: no crash, t2 unbroken, TEXSHOW+ZFUNC=7 shows the spray (incl. oriented quads)
renders sanely. NOTE: the per-pixel orientation vs GX is NOT isolated (parked wash + HUD drift confound
the whole-frame number) — correctness rests on the unit-tested decomp-faithful math + no-regression.

## UPDATE (2026-06-19 night) — STEP 4 cont DONE & verified: t5/t6 Stripe/StripeCross RIBBON

Ported the Stripe (t5) + StripeCross (t6) ribbon path. Files: `ngx_jpa_billboard.h`
(`jpa_stripe_basis` / `jpa_stripe_corners`, render_test `jpa_stripe`), ribbon branch in
`jpa_particle_native.cpp` `ov_jpa_drawparticle`. render_test 15/15.

### What landed (all decomp-faithful, offsets confirmed live)
- Stripe is an EMITTER-level visitor (one ribbon over the whole particle list, NOT per particle).
  Implemented as a RIBBON branch in the same drawParticle override (skips the per-particle quad loop
  when `shapeType==5||6`), emitting ONE quad per consecutive-particle SEGMENT, reusing
  `ngx_emit_particle_quad` (all texture/TEV/blend reused — no new emit fn). The GX_TRIANGLESTRIP
  [L0,R0,L1,R1,…] = segment quads [Li,Ri,Ri+1,Li+1]; the strip's two triangulations cover the same
  quad (cull=NONE so winding is moot) → mFlags reverse only flips per-particle v, which I handle.
- Per-particle rail math (`jpa_stripe_corners`): basis M=[axis|side|dir] (Gram-Schmidt from
  params.unk0 settled-axis + dir; **column order differs from jpa_dir_basis** which is [axis|dir|side]).
  rails v1=(x·sin,x·cos,0) v2=(y·sin,y·cos,0), x=−w(u4x+ucx) y=+w(u4x−ucx) [unk4.x for BOTH], w=unk10;
  edge = pt0 + M·v; eye = mViewMtx·edge; uv=(0/1, fVar2 running 0→1 step 1/(elems−1)).
- StripeCross 2nd ribbon: w=unk14, sin=−JMASSin/cos=JMASCos, dir = mVelocity DIRECTLY (not
  mDirTypeFunc), **fVar2 NOT reset** (carries from ribbon 1 → 2nd ribbon v≈1→2; replicated).
- COLOUR emitter-level (RegisterColorEmitterPE): C0=THRE(emitter.prm, cb.prm), C1=THRE(emitter.env,
  cb.env). emitter prm/env = JPADraw.mPrmColor@+0xB8/mEnvColor@+0xBC via JPADrawContext.unk14@JPADraw+0xA4.
- dirType (mDirTypeFunc) 0=vel(+0x38) 1=localpos(+0x20) 2=−localpos 3=emitter.mEmitterDirection(+0x210)
  4=prevParticle.globalpos−this (prev = embedded link mPrev @ particle+8; no-prev → 0→(0,1,0) fallback).
- New offsets: BS_FLAGS@baseShape+0x7C(bit0=reverse), list mTail@em+0xF8/mLinkCount@em+0xFC,
  JSULink mPrev@+8, P_UNK34@drawParams+0x34(u16, read as mem_r32>>16). JMASSin/Cos table ≈ sinf/cosf of
  u16·2π/65536 (faithful to the table's intent).

### Verified (the bar: unit tests + TEXSHOW geometry + no-regression, NOT whole-frame which is wash-confounded)
- render_test `jpa_stripe` PASS (basis orthonormality + column order + zero-dir fallback + rail point).
- DBG_JPA on spray_fwd: `[jpa-stripe] type=6 dir=0 elems=20 segs=38 tex=64x64 cc(C1,C0,TEXC,ZERO)=
  lerp(env,white,tex) c0=(255,255,255,255)` — branch fires, segs = 2 ribbons × (elems−1), texture
  resolves, faithful combiner. Type histogram still t2/t3/t6/t9 → no regression to the other types.
- TEXSHOW+ZFUNC=7 (scratch/screenshots/stripe_texshow.png): the spray renders the StripeCross as
  CONNECTED curved water-arc ribbons (not scattered dots). Drift-free /ngxnojpa A/B: particles
  contribute 3.7% (LEQUAL) / 13.8% (TEXSHOW) at the spray location. 130 s run, no crash/NaN.

### NEXT (step 4 long tail): Rotation (t7) / RotCross (t8) / RotBillBoard / RotDirectional variants,
YBillBoard (t10), per-particle texanim (unk3A), colour/alpha animation, child particles, ext/extra
shapes. (DirCross t4 / RotDirCross also unported but unseen in plaza.) Same verify recipe below.

## ASIDE (2026-06-19 night) — fixed a pre-existing ngx shader-gen bug found while verifying particles
`tev_shader.cpp:108` (indirect-texcoord warp) emitted `ivec3(round(texture(...)*255)).abg` — a `.abg`
swizzle (component 3 = alpha) on an ivec3 (only .rgb) → GLSL "swizzle out of range" → the WHOLE pixel
shader failed to compile (3×/run in plaza; those indirect materials fell back). Fixed: `ivec3(`→`ivec4(`
so `.abg` operates on a 4-component vector, matching Dolphin sampleTexture→int4 then `.abg`. Verified
plaza shader-compile errors 3→0, render_test 15/15, ab_oracle fresh_plaza 18.7% (baseline, no regress).
Committed 89e5e23. (Not particle-related — a renderer correctness fix.)

## UPDATE (2026-06-19 night, 2) — STEP 4 cont DONE: child particles (drawChild, FLUDD splash/mist)

Ported `JPADraw::drawChild` (the separate child-particle seam). Override @ **0x8032bf70** in
`jpa_particle_native.cpp`, mirrors the drawParticle override: run-original-first lets the guest
`setChildClipBoard` re-fill the static `cb` with the CHILD clipboard (own size cb.unk4, pivot cb.unkC=0,
fixed unit texcoords, the PNMTX0 model matrix cb.unk68) + child GX z/blend; then I walk the child list
(`emitter+0x100`) and emit billboards reading the CHILD cb. Reuses `billboard_corners` (render_test'd).

### Decomp-faithful details (offsets confirmed live)
- Child list = JPABaseEmitter.mChildParticleList @ **emitter+0x100** (mHead+0x100, count+0x108). VERIFIED:
  the list-walk count == the stored mLinkCount in every sample (walked==listCount, 1/1 & 2/2).
- Child uses the SAME JPADrawExec* as the parent, selected by **SweepShape type** (mSweepShape @
  JPADraw+0x9C; type@+0x44 dir@+0x45 texIdx@+0x4C scaleY@+0x10 scaleX@+0x14 prm@+0x38 env@+0x3C
  alphaOut@+0x4B inherit@+0x4E). Reachable plaza spray children = SweepShape type 2 (BillBoard) → emit
  screen-aligned billboards (other child types share the parent per-type math, addable later).
- PNMTX0 model matrix cb.unk68 @ cb+0x68 (Mtx 3x4): identity when the PARENT baseShape is BillBoard(2)/
  DirBillBoard(9), a viewMtx copy otherwise, YBB for type 10. I read it and apply it to every corner
  generally (identity for the reachable t2-parent case) so it's correct for all parent types.
- Child texture: `id = mTexIndices[sweepShape.getTextureIndex()]` (mTexIndices u16* @ JPADraw+0xB0), then
  decode unk2C[id] — refactored `decode_jpa_texid` shared with the parent's getMainTextureID path.
- Child colour: emitter-level RegisterColorChildPE (sweepShape prm/env THRE'd vs cb.prm/env) UNLESS the
  sweep enables alphaOut / inheritedAlpha(0x2) / inheritedRGB(0x4) → then per-particle RegisterPrmCEnv
  (child drawParams prm/env, a=mAlpha·THRE). Both implemented, keyed on the sweep flags (perPart).
- TEV combiner = the parent baseShape TevArgs (drawChild keeps the shape's combiner from draw()).

### Verified (live-spray, since children are FLUDD-transient — can't save them, see reachability below)
- Drive freeroam_plaza + hold /pad do=r → `[jpa-child] stype=2 listCount=2 walked=2 drawn=2 tex=32x32
  perPart=1`: override fires, walks the child list correctly, emits all, resolves the child texture.
- render_test 15/15 (the reused billboard math). Parent path UNREGRESSED (still tex 64x64). No crash, no
  NEW shader errors. NOTE: a pre-existing ngx TEV shader-gen bug (`'abg' swizzle out of range`, 3×/run)
  is present with OR without this change — a separate rare-combiner issue, not particle-related.
- Child count is sparse at this spray angle (1–2 live); the visual contribution is modest but correct.

### Step-4 remaining: Rotation (t7/t8), YBillBoard (t10), DirCross (t4), per-particle texanim, ext/extra
shapes, non-t2 CHILD types — all UNREACHABLE in plaza (need a scene that uses them; verify-first).

### REACHABILITY of the step-4 long tail (measured 2026-06-19 night — read BEFORE porting more)
Added a per-window reachability probe to DBG_JPA: `[jpa-types] … sweepEm=N childParts=N texanimEm=N`
(`sweepEm` = emitters with a SweepShape, i.e. drawChild candidates; `childParts` = LIVE child-particle
count; `texanimEm` = emitters in per-particle texanim mode). Measured in the plaza:
- **Idle plaza & spray_fwd.sav**: `childParts=0 texanimEm=0`; only t2/t3/t6/t9, no t4/t7/t8/t10. ⇒ ALL
  the step-4 long-tail types (Rotation/YBillBoard/DirCross/texanim) are **UNREACHABLE in plaza** —
  per the tooling-first rule, do NOT port them until a scene that uses them is found & verifiable.
- **Child particles (drawChild, the FLUDD splash/mist)**: reachable ONLY while ACTIVELY spraying at a
  surface (drive freeroam_plaza + hold /pad do=r → `childParts≈122` live). They are **very transient**
  (die <1s after the spray stops) and **do NOT survive a static save** (saved mid-spray, a 0.3–0.7s
  reload already shows `childParts=0`) — so the frame-exact save A/B can't catch them. ⇒ drawChild is
  verifiable only by LIVE continuous-spray TEXSHOW + DBG, not ab_oracle. It is also a sizeable sub-port
  (own clipboard `setChildClipBoard` @ JPADraw.cpp:854: child scale 25·sweepScale·emitter.unk174, a
  MODEL matrix cb.unk68 via GXLoadPosMtxImm — NOT pre-transformed to eye space like the parent path —
  sweepShape-driven types/dir/rot funcs, RegisterColorChildPE colour from sweepShape prm/env, child
  list @ emitter+0x100). Offsets gathered: mSweepShape @ JPADraw+0x9C, childList mHead@+0x100 count@+0x108.
- The deterministic spray_fwd ab_oracle = 20.5% but **uniform across all 16 regions** (62/62/60/55…) =
  the PARKED background wash, not a particle gap (spray_fwd is Mario AIMING, no dominant stream). The
  save A/B cannot isolate particle fidelity here — re-confirmed; rely on unit tests + TEXSHOW + /ngxnojpa.

NEXT-SESSION PLAN for drawChild: either (a) build a live continuous-spray verification harness (inject
held /pad, capture TEXSHOW each frame, confirm child mist geometry + DBG child-quad count, no t-regress),
or (b) find a scene with a PERSISTENT child-particle emitter (save-stable) before porting. Don't port
it blind — verify-before-done.

## UPDATE (2026-06-19 latest) — STEP 4 cont DONE: stype=6 StripeCross CHILD ribbon + parent-longtail dead-end

This session: per the respawn brief ("find a verifiable scene FIRST, then port what you can verify"),
did a thorough live reachability sweep of the plaza, then ported the ONE newly-verifiable long-tail item.

### Reachability sweep (live-driven, DBG_JPA `[jpa-types]` histogram) — measured, don't re-walk
Drove Mario all over the plaza via /pad (located Mario obj ptr = *(0x8040E10C); SMS_GetMarioPos =
`lwz r3,-0x60B4(r13)`, r13=0x804141c0; position @ that ptr +0 = TVec3, e.g. (-1556,300,4366) at the
freeroam_plaza spawn). Drove forward/turn/jump sweeps, into the lower water area (y 300→-78), past
coins/NPCs/buildings. **Result: the ONLY parent draw types that EVER occur in plaza are t2/t3/t5/t6/t9
(all ported); the ONLY child SweepShape types are stype=2 (done) and stype=6 (newly found, see below).**
**t4 (DirCross), t7 (Rotation), t8 (RotCross), t10 (YBillBoard), and per-particle texanim (texanimEm)
NEVER appear** — re-confirmed UNREACHABLE in plaza despite extensive driving (coins/water/jumping did
NOT spawn any). Per the tooling-first hard rule, they stay UNPORTED until a stage that uses them is
reachable+verifiable (would need a save made after driving into a level; fastboot only reaches plaza).
**DEAD END for cheap verification — documented, not ported blind.**

### NEW reachable + verified: stype=6 StripeCross CHILD ribbon (a persistent water-feature emitter)
While driving the lower plaza (around (-3434,-80,6150) / (-3918,300,5876), a water feature), found a
**persistent** child-particle emitter: `childParts≈120 child[ptype=6 stype=6]` SUSTAINED while idle
(unlike the FLUDD-spray children which are spray-transient). ⚠ It still does NOT survive a static save
(`water_children.sav` reloads with childParts=0 — the live emitter doesn't respawn its children on
reload; a loadstate of it also hit the known OS backpressure wedge, pc=idle 80002ff0, unrelated to my
code). ⇒ verify it LIVE: load freeroam_plaza.sav, drive the up/left/down/right×2 sweep → childParts>0.

The old drawChild override drew ALL children as per-particle billboards, but stype=5/6 children are an
EMITTER-LEVEL RIBBON (JPADrawExecStripe/StripeCross run in the drawChild `unk18` loop; the per-particle
`unk70` set is empty for ribbons — JPADraw.cpp:1040-1059). Ported it:
- **Extracted a shared `emit_stripe_ribbon(...)` helper** (jpa_particle_native.cpp) used by BOTH the
  parent ribbon (drawParticle) AND the child ribbon (drawChild) — the JPADrawExec is identical, only
  the clipboard/list/transform differ. Parent passes viewMtx + parent list + baseShape dirType; child
  passes **cb.unk68 (the PNMTX0, = a viewMtx copy for stripe/default parents per setChildClipBoard
  JPADraw.cpp:856-867)** + child list (emitter+0x100/0x104/0x108) + **sweepShape dirType (sweep+0x45)**.
  Faithful: using cb.unk68 = exactly the GPU PNMTX0 the child exec's world coords pass through.
- Child ribbon colour = emitter-level RegisterColorChildPE (sweep prm/env THRE cb prm/env — already in
  the override; recomputed inside the ribbon branch for the perPart case so it's never left black).
- The child Stripe exec reads the SAME per-particle fields (unk34 rot, unk10/unk14 width, unk0 axis,
  velocity) and clipboard unk4.x/unkC.x — confirmed identical to the parent exec (JPADrawVisitor.cpp
  L1117/L1193 vs setChildClipBoard).

### Verified (the bar: render_test + live DBG + no-regression + no-artifact, NOT whole-frame wash)
- **render_test 15/15** (reuses `jpa_stripe` — the corner math is unchanged/shared).
- **Live**: `[jpa-child-ribbon] stype=6 dir=0 elems=2 segs=2 tex=0x809bb3e0 32x32` — the NEW branch
  fires, resolves the child texture (valid 32×32), emits segments, the old `[jpa-child]` billboard path
  no longer handles stype=6. **No crash** over the run.
- **No-regression**: parent `[jpa-stripe] type=6 … segs=38` unchanged after the helper refactor; the
  type histogram still t2/t3/t5/t6/t9.
- **No transform artifact**: a TEXSHOW (opaque) ngx capture renders the plaza cleanly with NO wild-span
  streaks across the frame (a broken child M68 transform would streak the opaque ribbon). The children
  are sparse here (elems=2 → small water mist), so not prominent in a full-frame shot, but the geometry
  path is the SAME helper already TEXSHOW-verified to draw curved water-arc ribbons for the parent.

### STILL UNREACHABLE/UNPORTED (need a non-plaza scene; verify-first before porting):
Rotation (t7/t8), YBillBoard (t10), DirCross (t4), per-particle texanim (unk3A), ext/extra shapes,
non-{2,6} child types. To reach: drive into a level (Bianco/Ricco/etc.) and /savestate there — hard
headless (precise portal navigation). Sparkle/coin/star effects are the likely t7/t10 sources.

## UPDATE (2026-06-19 latest+1) — ★ REACHABILITY UNLOCK + YBillBoard (t10) ported & verified

### ★ THE UNLOCK: fastboot into ANY stage via SUNBRIGHT_STAGE (the parent long-tail is now reachable)
fastboot already supports **`SUNBRIGHT_STAGE=<n> SUNBRIGHT_SCENARIO=<n>`** (fastboot_native.cpp:243-262,
the DEBUG ROOM path) to boot straight into an arbitrary stage instead of Delfino Plaza — no level-portal
navigation needed. **`SUNBRIGHT_STAGE=2 SUNBRIGHT_SCENARIO=0` boots into Bianco Hills**, runs to an
interactive core (emu_secs>12), and is save-stateable (`scratch/stage2_ybb.sav`). This makes the
previously-"unreachable in plaza" parent long-tail VERIFIABLE — drop the plaza-only constraint.
- Stage 2 = Bianco Hills. Its JPA type histogram: **t2/t3/t6 + t10 (YBillBoard, ~7989 persistent)** +
  stype=2 children. (Still no t4/t7/t8 here — try other stages: ricco/gelato/pinna/sirena/noki/pianta
  are higher indices; sparkle/coin/star effects are the likely t7/t10 sources.)
- ⚠ Booting a level is uncharted under no-recomp (first non-plaza ngx render). Bianco renders the
  village/path/trees fine but has large WHITE BLOBS present WITH AND WITHOUT JPA (= NOT particles; a
  separate Bianco wash/sky/untextured-geometry gap — out of scope for N7, note for later).

### YBillBoard (t10) — ported & verified (the first formerly-unreachable parent long-tail type)
JPADrawExecYBillBoard (JPADrawVisitor.cpp L391) + loadYBBMtx (JPADraw.cpp:1200) + setParticleClipBoard
case 10. The quad stays vertically upright but tilts in the camera Y/Z plane. Derived (disasm/decomp):
- PNMTX0 = identity (loadYBBMtx sets cb.unk68=Identity). Position = FULL eye-space pt = viewMtx·globalPos
  (the exec's `MTXMultVecSR(viewMtx,pt)` + the unk38 translation column = viewMtx[*][3] recombine to the
  full transform — proven analytically). Corners add the local offsets rotated by unk38, whose rotation
  part is rows [1,0,0],[0,vy,-vz],[0,vz,vy] with **(vy,vz)=normalize(viewMtx[1][1], viewMtx[2][1])**.
  Since offs.z=0 this collapses to **corner = (pt.x+ox, pt.y+vy·oy, pt.z+vz·oy)**.
- Offsets ox/oy use scaleX=unk10, scaleY=unk14, clipboard unk4/unkC — same layout as billboard_corners.
- Pure math `jpa_ybillboard_corners` in ngx_jpa_billboard.h; wired into the per-particle branch
  (shapeType==10) in ov_jpa_drawparticle, before the billboard fallback (t10 was falling to the wrong
  screen-aligned billboard before).
- VERIFIED: **render_test `jpa_ybillboard` (16/16)** — 3 hand-computed cases (tilt, the (0,2)→(0,1)
  normalization, level-cam vy=1/vz=0 → constant-z upright). Live stage 2: t10=7989 handled, no crash;
  drift-free /ngxnojpa A/B shows JPA particles contribute 5.4% of the frame; Bianco renders cleanly with
  no wild-span transform artifact. (Per-pixel t10-vs-GX not isolated — same wash/no-oracle trap; rests on
  the unit-tested decomp-faithful math + no-regression + renders, the established bar.)

### COMPREHENSIVE reachability sweep (2026-06-19, SUNBRIGHT_STAGE=2..11 + active play) — READ before more
Swept stages 2-11 (scenario 0) with DBG_JPA, plus active driving/spraying/jumping in Bianco (stage 2)
and Delfino. **The complete set of JPA draw types that occur in any cheaply-reachable scene:**
- Parents: **t2 BillBoard, t3 Directional, t5 Stripe, t6 StripeCross, t9 DirBillBoard, t10 YBillBoard**
  — ALL PORTED & verified. (Per-stage: s2 Bianco=t2/t3/t6/t10; s5=t2/t3; s7=t2/t6; s8=t2/t6; s9=t2/t3/t9;
  s3/s4/s6/s10/s11=t2-mostly. Delfino plaza=t2/t3/t5/t6/t9.)
- Children: **stype=2 BillBoard, stype=6 StripeCross ribbon** — ALL PORTED & verified. (Combos seen:
  ptype2/stype2, ptype6/stype6, ptype6/stype2.)
- **NEVER seen anywhere cheaply: t4 DirCross, t7 Rotation, t8 RotCross, per-particle texanim (unk3A=0
  everywhere), ext/extra shapes.** These are EVENT-GATED (boss fights / fireworks / specific objects/
  cutscenes), NOT used in normal level traversal. Per verify-first they stay UNPORTED until a scene that
  uses them is reachable.

### NEXT (the remaining N7 long-tail is gated behind GAMEPLAY-EVENT reachability tooling)
To port t4/t7/t8 you must first REACH a scene that uses them (verify-first). Candidates + the tooling gap:
- **Boss fights** (Petey/Gooper/Wiggler/etc.) — likely Rotation (goop bursts, splatter). Reaching one
  headless = drive into the arena trigger (precise multi-step nav + maybe combat). Try boss-EPISODE
  scenarios (SUNBRIGHT_STAGE=2 SUNBRIGHT_SCENARIO=<petey-ep>) — may drop nearer the arena, but the
  fight still needs a trigger. NEEDS: a scripted-navigation / Mario-teleport(write *(0x8040E10C)+0) harness.
- **Pinna Park fireworks** (event-timed) — classic Rotation source.
- When reached, port the Rotation family the SAME way as t10: RotBillBoard (JPADrawVisitor L353) /
  RotYBillBoard (L428) / Rotation (L973) / RotationCross (L1014); the rotType matrices rotTypeY/X/Z/XYZ/
  YJiggle at L507-600; DirectionalCross (L744). render_test unit FIRST, then wire into ov_jpa_drawparticle.
- ⚠ This is a SIZEABLE tooling effort (gameplay automation) for visually-minor types — weigh against
  other ngx work. Mario obj ptr = *(0x8040E10C); position = that ptr +0 (TVec3 f32); SMS_GetMarioPos =
  lwz r3,-0x60B4(r13), r13=0x804141c0. SUNBRIGHT_STAGE/SCENARIO fastboots into any stage (the unlock).

### NEXT (historical, DONE above): **t6 StripeCross (~549, the biggest non-t2)** — RIBBON, fully RE'd below
EMITTER-level visitor (runs ONCE per emitter over the whole particle list, NOT per particle).
JPADrawExecStripe (t5) @ JPADrawVisitor.cpp L1117 / StripeCross (t6) @ L1193. Plan: do it in the
SAME drawParticle override but as a RIBBON branch (skip the per-particle quad loop). Reuse
ngx_emit_particle_quad PER SEGMENT (one quad between consecutive particles) so all the
texture/TEV/blend machinery is reused — no new emit function needed.

Per-particle ribbon edge (both edges of the strip), for particle i:
- elems = emitter particle-list count (emitter+0xF4 list; need ≥2). Iterate first→last, or last→first
  if (mBaseShape.mFlags & 1) (mFlags @ baseShape+0x7C). v-coord fVar2 runs 0→1 (step 1/(elems-1)),
  or 1→0 reversed.
- sin/cos = JMASSin/Cos(params.unk34)  [unk34 @ drawParams+0x34, the rotation angle, u16 fixed].
- width: x = -unk10*(u4x+ucx), y = +unk10*(u4x-ucx)  [NOTE: u4.x for BOTH; ribbon uses unk4.x only].
  v1 = (x*sin, x*cos, 0), v2 = (y*sin, y*cos, 0).
- dir = mDirTypeFunc(particle) (vel/localpos per mDirType); if zero → (0,1,0); else normalize.
- side = cross(unk0, dir); if zero → (0,1,0); else normalize. unk0 = cross(dir, side); normalize.
  (unk0 = drawParams+0x0, already settled by the original guest draw — READ only.)
- basis M = columns [unk0 | side | dir]; world edge = pt0 + M·v1 (left) and pt0 + M·v2 (right).
  (rows: u1=(unk0.x,side.x,dir.x) etc; vertex.x = pt0.x + v.dot(u1), .y +v.dot(u2), .z +v.dot(u3).)
- eye = mViewMtx · world edge. texcoord left=(0,fVar2) right=(1,fVar2).
Emit a quad per segment i→i+1: corners [Li,Ri,Ri+1,Li+1], uv [(0,vi),(1,vi),(1,vi+1),(0,vi+1)].
StripeCross (t6) draws a SECOND perpendicular ribbon (L1270): width from unk14 (not unk10),
sin=-JMASSin / cos=JMASCos, dir = particle.mVelocity DIRECTLY (not mDirTypeFunc); ⚠ fVar2 is NOT
reset between the two ribbons in the decomp (it keeps incrementing → 2nd ribbon's v is offset) —
replicate that (don't reset) for fidelity, or test both.
COLOUR for stripe is EMITTER-level (RegisterColorEmitter*, sets one TEVREG0), not per-particle
RegisterPrmColorAnm — use cb.mPrmColor (epr..epa, already read) as C0 and cb.mEnvColor as C1; alpha
from the emitter prm. (Per-particle prm/alpha is NOT applied by the stripe path.)
Unit-test the pure ribbon-edge math (basis + M·v + segment quad winding) in render_test before wiring.
Then: Rotation (t7/t8), YBillBoard (t10), per-particle texanim (unk3A), colour/alpha anim, children.

### (historical) NEXT — step 3: particle TEXTURE + the shape's full TEV (the big visual win)
Flat PASSCLR dots ≠ water. Add: decode the particle texture (JPABaseShape → texture index → JPA
resource JUTTexture → ResTIMG → reuse N1 sb_tex_decode → bind), the 4 quad texcoords
(cb.mTexCoords[4] @ cb+0x14, 8 bytes each), and the shape's TEV (GXSetTevColor TEVREG0=prm/TEVREG1=env
per emitter, JPADrawVisitor RegisterPrm*). The texture is what makes the spray read as soft water.
JPADrawExecLoadTexture (JPADrawVisitor.cpp L175/L311) shows the texture-resource load path; the
texcoords are already in cb.mTexCoords (set per shape). Then re-verify spray_fwd vs GX oracle.

### Verify recipe (this session)
- Make a forward-spray save: load scratch/freeroam_plaza.sav, /pad?do=r (spray), /savestate. Done:
  scratch/spray_fwd.sav.
- 3-way: capture ngx (NGX_PRESENT=1), ngx NO_JPA (baseline), GX (NGX_PRESENT=0) all via /loadstate of
  the same save; diff WITH vs WITHOUT (particle px) and each vs GX (should move toward GX).
- /ngxpresentlive reports ngx_particles: emitted_quads/tris + last_ti.

## UPDATE (2026-06-19 latest+2) — ★★★ DATA-VERIFIED: t7/t8 Rotation are DEAD CODE in SMS (quest closed)

The previous handoff asked the next session to "reach a scene that uses t7/t8 (Rotation), verify it
appears, then port the Rotation family." Instead of an open-ended scene hunt (which had already failed
across ~30 stage indices/scenarios), I closed the question from DATA with a new verify-first tool:

**`tools/render/jpa_shapetype_census.py`** decodes the global `/data/particle.szs` (Yaz0→RARC, 268
emitters) AND all 108 `/data/scene/*.szs` archives, finds every `BSP1` block, and counts the shapeType
byte (`mType = data[0x24]` from the 'BSP1' magic — JPABaseShape.cpp:102). Run:
`python3 tools/render/jpa_shapetype_census.py <rom.rvz>` (or `--dir scratch/scenes` on an extracted tree;
extract with `sunbright-jingle <rom> --extract /data/scene/ <dir>`).

**GAME-WIDE census (particle.szs + all 108 scenes):**
```
  t0  Point          : 8       t5  Stripe         : 17
  t1  Line           : 8       t6  StripeCross    : 66
  t2  BillBoard      : 935     t7  Rotation       : 0   <-- ZERO, game-wide
  t3  Direction      : 679     t8  RotationCross  : 0   <-- ZERO, game-wide
  t4  DirectionCross : 63      t9  DirBillBoard   : 154
                               t10 YBillBoard     : 11
```

### Consequences (do NOT re-chase Rotation)
- **t7 Rotation / t8 RotationCross occur ZERO times anywhere in the shipped game.** They are dead code in
  the SMS JPADrawVisitor dispatch. No reachable OR unreachable scene uses them — there is nothing to
  reach, and porting them would be unverifiable dead code. The multi-session "Rotation" frontier is
  CLOSED by data. Candidates the handoff suggested (Corona lava, Pianta fire, Pinna fireworks, bosses)
  were all chasing a type that does not exist in the data — abandoned correctly.
- **t4 DirectionCross (63 emitters) DOES exist and is still UNPORTED** — and crucially it is present in
  `dolpic5.szs` (the DEFAULT Delfino Plaza, t4=2) plus mamma/mare/ricco/pinnaBeach/pinnaParco6/bosses.
  But it does NOT actively draw at fastboot or in general plaza play (the earlier sweep saw t2/t3/t5/t6/t9
  only). So t4 is present-in-archive but OBJECT/EVENT-gated: porting it needs a way to make a t4 emitter
  fire + DBG_JPA confirm it (verify-first). Same gameplay-automation gap as before, now for a type that
  is real (not a phantom) — 63 emitters across many levels, worth doing once triggering tooling exists.
- t0 Point (pinnaParco only) and t1 Line (mamma only) are rare and also event/scene-specific.

### Bounded-hunt verdict + pivot
Per the handoff's own ALTERNATIVE clause: the Rotation hunt is now a PROVEN dead end (data, not just a
failed sweep). Pivoting to the OTHER ngx gap the prior session flagged: **non-plaza levels render large
WHITE BLOBS under ngx (Bianco etc.), present with AND without particles** = a non-particle wash/texture/
sky gap (ngx's first time rendering a level). Verifying that observation myself next, then diagnosing.

## UPDATE (2026-06-19 latest+3) — file-select oracle tool + evidence-based diagnosis (user redirect)

User redirected off levels back to FILE-SELECT (work-order: title+file-select before Delfino) and said
"build tools to do oracle compare." Built it:
- **`tools/render/fs_oracle.sh` + `tools/render/img_avg.py`** (commit 4529c5f). Captures N frames each of
  ngx (NGX_PRESENT=1) and the Dolphin-GX baseline (NGX_PRESENT=0), both held at file-select via
  `/pad?do=autostop`, TIME-AVERAGES each side to cancel animation phase (clouds/water/running Mario =
  the documented artifact, memory fileselect-cloud-wash-drift-artifact), then per-region diff+heatmap via
  ab_diff.py. img_avg refuses an all-black average. Pinned to build-freshtest (SUNBRIGHT_BIN overrides).

### What the phase-robust oracle proves (NOT artifact — the prior "faithful here" note is WRONG now)
Time-averaged whole-frame mean RGB: **GX (108,155,190) vs ngx (89,120,154)** — a real, systematic ~0.8×
darkening, dominated by the sky/sea/far background. Heatmap also shows doubled text/windows = a 2D offset.
Concrete issues, ranked:
1. **Background (sky/sea/palm-tree, all FAR vertex-colored materials) ~0.8× too dark.** **Sand is
   PIXEL-IDENTICAL (235,202,174)** → NOT a global gamma/multiply; specific to the far materials.
   Ruled OUT with evidence (via /shapeat + /gxstate?ti=11 on the sea shape sh=80e84cd4):
   - NOT the blend: force-opaque (/ngxnoblend?on=0) leaves sea dark (49,97,141) vs GX (89,165,235).
   - NOT lighting: sea has nrmcls=0 (no normals); enabling lighting would BLACKEN, not brighten.
   - NOT fog: GXSetFog SYNC tee = type 0 (GX_FOG_NONE). Fog is OFF in this scene.
   - NOT the shader: generated GLSL is a clean RASC passthrough (COLOR ADD a=RASC, b/c/d=ZERO → vtx color).
   - NOT the blend-factor mapping: ngx vk_dst maps GX INVSRCCLR→ONE_MINUS_SRC_COLOR correctly (ngx_present.cpp:694).
   - REMAINING cause: the sea/sky draw with **src=ONE dst=INVSRCCLR (screen blend)** so result =
     frag + bg·(1−frag); the LOWER layers of the multi-layer blend stack are darker in ngx, propagating
     up. This is the documented multi-layer-blend NO-ORACLE trap — needs layer-by-layer isolation of the
     bottom sky layer / clear. ⚠ xfmem claims COLOR0 en=1 mask=03 for the sea but the J3D object block
     says en=0 (faithful) — that's the async-lag liar (memory xfmem-not-cpu-oracle), do NOT chase it.
2. **J2D window vertical offset ~11px** (window top: GX y=58 vs ngx y=69) + left-edge/corner differs.
   A 2D ortho/viewport positioning bug — independent of the blend stack, more tractable.
3. **Extra Mario at the TOP of the frame** in ngx (persists through time-averaging → a real consistently-
   drawn model, not a jump); GX has only the bottom running Mario.

NEXT (needs user steer on which is THE "serious" one, given this screen's thrash history): (2) window
offset and (3) extra Mario are concrete/tractable; (1) is the blend-stack trap. Use fs_oracle.sh for any
fix's verdict (it's the trustworthy number now).

## UPDATE (2026-06-19 latest+4) — FRAME-EXACT file-select oracle: all 3 bugs CONFIRMED REAL

Made a file-select save (scratch/fileselect.sav, CURRENT build) and ran the EXISTING frame-exact
ab_oracle.sh with it (`SUNBRIGHT_BIN=build-freshtest/sunbright ./tools/render/ab_oracle.sh
scratch/fileselect.sav 4`) — both processes loadstate the IDENTICAL save → camera/clouds/water/Mario
byte-exact, NO drift confound. Result: **MEAN delta 22.7% (640x448), persists frame-exact** → the
file-select differences are REAL renderer bugs, not the documented animation-phase artifact. Per-region
grid: window rows dominate (68–102), top sky ~40–57, bottom-right (OPTIONS) 66.

Frame-exact spot samples (trustworthy now): sand PIXEL-IDENTICAL (235,203,174); but sky
(29,139,225→116,133,149), sea (130,183,242→69,105,145), palm-trunk (→near-black) all dark/desaturated
in ngx; windows close (slightly more saturated). Artifacts saved: scratch/screenshots/abo_oracle.png
(GX), abo_ngx.png (ngx), abo_wash.png (heatmap).

### THREE confirmed file-select bugs (fix these; verify each with ab_oracle.sh scratch/fileselect.sav)
1. **GHOST MARIO (extra Mario at TOP).** PRECISELY LOCALIZED: the file-select frame has 3 projection
   passes (g_proj_pass: 6=16 shapes, 7=17 [sky/sea bg], 8=26). The SAME Mario shapes (e.g. sh=80ea09c0
   nv=616, sh=80ea0fb8, sh=80ea0cd0 …) are drawn under BOTH pass=6 (ndc y~+0.7 BOTTOM = correct, matches
   GX) AND pass=8 (ndc y~-0.8 TOP = the ghost). The pass=8 copies are LISTED TWICE in /ngxshapes (a
   capture/publish buffering duplication); pass=6 once. GX with identical RAM shows ONLY the bottom Mario.
   efbcopies=0 → NOT an EFB-copy auxiliary epoch (so RTFILTER can't catch it; it's a different cause).
   → Investigate ngx_j3d_shape.cpp capture/publish (rec.pass set @~1793, g_proj_pass++ @~2323, double-
   buffer publish at J2DScreen::draw boundary). Hypothesis: stale/duplicated pass-8 geometry from the
   prior frame's buffer is published into this frame. FIX = don't emit the duplicated pass-8 Mario.
2. **BACKGROUND DARKENING (sky/sea/palm ~0.8×).** Real (frame-exact). Sky = TEVC×RASC (CMPR tex, tiled
   ~69×, screen-blend src=ONE dst=INVSRCCLR); sea = RASC passthrough, screen-blend. RULED OUT with probe
   evidence: blend (force-opaque still dark), lighting (no normals→would blacken), fog (GXSetFog tee
   type=0 OFF), shader (clean passthrough), blend-factor map (INVSRCCLR→ONE_MINUS_SRC_COLOR correct),
   texture decode (/tex PARITY-OK 119/119), draw-order/camera-drift (frame-exact). REMAINING: the
   multi-layer screen-blend STACK composites darker — likely coupled to the multi-pass (6/7/8) issue
   (pass-7 bg drawn between two Mario passes; draw-order/pass interaction). Suspect the same pass/buffer
   bug as #1 corrupts the screen-blend accumulation. Investigate after #1 (may fix both).
3. **Windows slightly off** (minor; largely a consequence of #2 — translucent over dark bg).

### Tools built this session (committed): tools/render/jpa_shapetype_census.py (t7/t8 dead-code proof),
tools/render/fs_oracle.sh + img_avg.py (time-averaged FS oracle). For FRAME-EXACT use ab_oracle.sh with
scratch/fileselect.sav (preferred — no drift). USER DIRECTIVE this session: file-select before Delfino;
stop asking, use the oracle + build tools, just fix it.

## UPDATE (2026-06-19 latest+5) — BUG 1 (ghost Mario) FIXED; BUG 2 (wash) re-diagnosed frame-exact

Continued the file-select bug work (handoff scratch/handoff_next.md). Built per-shape/per-batch
GXSetViewport(0x803630c8)/GXSetScissor(0x80363138) capture (vp/sc now in /ngxshapes), + /ngxdroppass
/ngxonlypass probes (NgxRenderBatch gained pass + vp_w/vp_h).

### BUG 1 — GHOST MARIO = uncomposited OFFSCREEN render-to-texture pass. FIXED (b46dfb3).
The "ghost Mario at top" is the file-panel 3D PREVIEW. GX renders it into a **256×256 OFFSCREEN
viewport** (GXSetViewport vp=0,0,256,256), GXCopyTex's it to a texture, composites it into the panel
(small/hidden for these slots). ngx has no RTT path → drew that geometry DIRECTLY full-frame at the
top = the ghost. PROVEN: on a frozen frame, the ONLY pass with vp=256×256 is the ghost; /ngxdroppass
on it removes the ghost and keeps the correct bottom menu Mario (pass-8, full vp).
Why the existing epoch/gen render-target model didn't catch it: the EFB-copy tracking overrides
(ov_efb_native_copytex 0x8035ee5c, ov_gxs_copydisp 0x8035ecec) are plain SUNBRIGHT_OVERRIDE — NOT
purejit-safe — so under no-recomp they fire ONCE (Dolphin caches passthrough); ngx_note_efb_copy never
runs per-frame → efbcopies=0, display_epoch model inert. (Don't make GXCopyDisp purejit-safe: a HW
override of it DEADLOCKS — memory ngx-n7-pe-block.)
FIX (principled, reads the game's own viewport state — NOT a magic constant): present drops any batch
rendered into a SUB-DISPLAY viewport (vp area < the frame's largest = main-scene display viewport).
A sub-display viewport IS the game's render-target extent for an offscreen RTT pass ngx can't
composite. Gated by rtfilter (default on). Verified frame-exact (ab_oracle scratch/fileselect.sav):
ghost gone, bottom Mario intact. No Delfino regression (fresh_plaza.sav 18.6%). render_test 16/16.

### BUG 2 — BACKGROUND WASH: NOT an artifact, NOT coupled to #1. It's mis-rendered cloud/overlay quads.
Fixing #1 did NOT fix #2 (delta still 22.6%; they're separate roots). pixbatch/pixblend at the sky
(NDC -0.7,-0.7) and sea/sand localize it CONCRETELY (refutes the old "wash=artifact" note, memory
fileselect-cloud-wash-drift-artifact, for the FRAME-EXACT case):
- The draw-order WINNER over the sky/sand is a big GREY overlay quad (this frame ti=1318 tex0=80a61ce0
  and ti=1321 tex0=80a65ce0, shapes sh=80d39254 / sh=80d390e0 — the sh=80d39xxx cluster). These quads
  have ENORMOUS NDC extents (x[-1.74,3.16], y[-0.75,2.09]) and HUGE clip-w (28307..64334 vs normal
  ~900) → they SPRAWL across the whole screen in ngx. In GX they're confined → ngx MIS-PROJECTS /
  OVER-COVERS them. (Matches the prior divergence-scan finding "ti=10 covers 62% screen vs GX puff".)
- The ti=10 cloud layers render bm=0 (OPAQUE) greenish (frag≈0.50,0.73,0.60 from a warm-white texture
  tx80a70780 (239,214,198) modulated by a greenish vtx color clr0≈(122,216,194)) → opaque greenish
  clouds replace the blue sky. GX clouds are WHITE sparse puffs. So per-material TEV color is ALSO off.
- The overlay textures are REAL ROM cloud textures (texat 80a65ce0 fmt=2 → sparse IA, intensity 0..85
  mean 16), NOT EFB-copies. So the fix is NOT "drop them" (BUG-1 style) — it's correct per-material
  TEV + projection. ⚠ pixblend's CPU "FINAL predicted" (white) does NOT match the GPU render (grey) —
  the CPU rasterizer doesn't model the real Vulkan blend; trust per-layer frag/tex/uv, not its FINAL.

### TOOLING GAP for BUG 2 (build this next): tev_index is UNSTABLE across frames (the per-frame
tevstate table is rebuilt → ti shifts, e.g. 1279↔1316↔1318 for the same material), so /gxstate?ti=N
and /ngxskipset?ti= only work on a FROZEN frame and can't be scripted across frames. Need /gxstate +
TEV/projection dump keyed by SHAPE ADDRESS (sh=80d39xxx is stable). Then pin the cloud quad's
projection (why huge-w/sprawl) and TEV (why greenish/opaque) vs GX, and fix per-material.
NEXT: with by-shape tooling, RE the file-select cloud/overlay draw (sh=80d39xxx) projection + TEV,
verify each fix with ab_oracle scratch/fileselect.sav (drop the 22.6%).

## UPDATE (2026-06-19 latest+6) — BUG 2 tooling (by-shape /gxstate) + status: it's COMPOSITING

Built by-SHAPE-ADDRESS /gxstate targeting (`/gxstate?sh=ADDR`, `sb_ngx_set_gxstate_sh`) so a material
can be analyzed ACROSS frames despite tev_index renumbering (the blocker noted in latest+5). Shape
addrs are stable for a given save (80d39254/80d390e0/80d391d8/80d392d0 = the file-select wash quads).

Analyzed the biggest wash quad sh=80d39254 (nv=545, tex 80a61ce0 64x128 fmt=5, the file-select water):
TEV = modulate (TEXC*RASC), alpha-test GEQ 128, blend mode=0 (opaque), zmode LEQ, UV0 in [0,1] (NOT
heavily tiled — the "tiled 69x" is a DIFFERENT shape). The gxstate DIFFs (ngx cc=0706/mat=REG vs xfmem
070f/VTX; ngx color_env 08f8af vs bpmem 08afff) are the **async-lag liars** (memory xfmem-not-cpu-oracle;
the tool itself caveats bpmem is GP-thread-lagged). ngx's OBJECT-MODEL reads are faithful — re-confirming
the prior session's "all per-material INPUTS faithful → COVERAGE/COMPOSITING bug".

⇒ BUG 2 is a multi-layer COMPOSITING/coverage emergent issue, NOT a per-material input bug. The only
valid oracle is final PIXELS (ab_oracle), NOT gxstate/xfmem/bpmem diffs. Per-region ab_oracle delta
(scratch/fileselect.sav) shows the WORST error is the WINDOW/PANEL band (mid rows 60-102), not the sky
(40-57); sand is near-exact (14-26). So "background darkening" is only part of it; windows/panels dominate.
This is the documented multi-layer-blend no-pure-oracle trap — but now attackable with the frame-exact
pixel oracle + the layer-isolation probes (/ngxonly /ngxskip /ngxprefix /ngxdroppass + freeze).
NEXT (a dedicated pixel-oracle campaign, fresh context): bisect the screen-blend stack layer-by-layer
with /ngxprefix on the frozen file-select frame, find which layer's compositing diverges from GX, fix
the blend/coverage there, verify each step drops the ab_oracle number. Don't trust gxstate DIFFs here.

## UPDATE (2026-06-19 latest+7) — BUG 2 narrowed via data-divergence to LIT-material under-brightness
USER directive: a wrong on-screen value IS engine work; find the data divergence, narrow, RE/port/tool.
Did exactly that on the file-select wash (frame-exact ab_oracle scratch/fileselect.sav, 22.5%):

METHOD: /ngxprefix layer-bisect on the GPU (pixblend's CPU rasterizer is UNRELIABLE — affine vs GPU
perspective-correct interp; it predicted white where the GPU renders dark — do NOT trust its FINAL).

FINDINGS (sampling abo_oracle.ppm[GX] vs abo_ngx.ppm[ngx] at fixed pixels):
- **UNLIT elements are PIXEL-EXACT**: sand (239,213,189)==(239,213,189).
- **LIT (en=1) elements render ~0.5x too dark**: A/B/C blocks (cc=0686) GX=(80,198,226) ngx=(37,107,131)
  ratio ~0.5; options text GX=(123,59,0) ngx=(49,24,0).
- **Translucent windows inherit the dark bg behind them**: corrupt/new panels GX=(98,102,254)
  ngx=(28,30,233) — R,G crushed because the sky/sea behind is dark in ngx (BUG 3 = consequence of BUG 2).
- The SKY per-pixel is CONFOUNDED by cloud texmtx animation (the sky pixel varied 28,88,138 vs 55,102,141
  across runs from the SAME save — the documented cloud-scroll). Use the static UI (lit blocks) as the
  clean signal, NOT the animated sky. The whole-frame MEAN (22.5%) is still robust.
- A specific foliage batch (ti=10 cc=070e OPAQUE) blackens the sea-left (0,65,89)->(42,30,8): also a
  lit/en material rendering dark.

ROOT-CAUSE LEAD (named, not yet fixed): ngx's per-vertex lighting (light_vertex, cc en=1 path) is
UNDER-BRIGHT by ~2x for the file-select lit materials → every lit element (blocks, foliage) is ~0.5x,
and the dark 3D bg bleeds through the translucent menu windows. Unlit (sand, vertex-color passthrough)
is exact, which is why the wash looked "global" but spares the sand. NEXT: compare light_vertex's
ambient+diffuse sum for a lit block (cc=0686) vs the GX result; the deficit is likely the ambient
source/scale or a missing light contribution (the N6 lighting model). Verify any fix with ab_oracle.

TOOLING NOTE: GX-side /ngxdrawlimit lockstep is UNRELIABLE under no-recomp (the GX XFB shows the last
complete frame regardless of the limit — it's only re-copied at GXCopyDisp), so per-layer GX gating
doesn't work; the ngx side gates fine. For GX ground truth use the full-frame two-process oracle.
