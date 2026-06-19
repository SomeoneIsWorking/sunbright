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
