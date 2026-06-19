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
