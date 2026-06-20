# 2026-06-20 — Pollution/graffiti goo (Sirena/Delfino) native-ownership RE (findings + plan)

Goal reframed by user: "not to replicate GameCube/GX — make a PC game engine." So own the goo as a
native engine effect, NOT by reproducing the GC EFB→R8 round-trip.

## Symptom
Sirena Beach (SUNBRIGHT_STAGE=6) GX baseline = floor/water covered in bright green/yellow "Manta
Storm" goo. ngx renders the scene (after the clear-aware-gen fix) but the goo is ENTIRELY ABSENT —
plain sand floor. Same class as the Delfino plaza pollution (the PARKED "wash"). `/ngxrtfilter?on=0`
(show ALL gens/epochs) does NOT reveal the goo → the goo is not in ANY captured ngx batch.

## How the goo works (reference/sms RE)
- The visible goo = **TPollutionLayer** planes (`Map/PollutionLayer.cpp`): a `TJointModel` (J3D model,
  drawn via `mActor->perform`) whose material samples a per-layer **coverage texture** `unk58`
  (ResTIMG, R8/intensity). Image data ptr = `unk54 = (u8*)unk58 + imageDataOffset`. The goo shows
  where coverage > 0.
- The coverage is NOT baked in the model. `TPollutionLayer::initTexImage` (PollutionLayer.cpp:55):
  for `gpMarDirector->mMap == 9` it seeds coverage from a depth map; **for every OTHER stage it sets
  `unk54[...] = 0`** (line 78-79) — coverage starts EMPTY.
- The live coverage is produced by the **"落書きグループ" (graffiti group)** offscreen pass set up in
  `TMarDirector::initECTGft` (System/MarDirectorInitECT.cpp): the goo shapes are drawn into the EFB,
  then a `TEfbCtrlTex("graffito check")` does a **GX_CTF_R8 (fmt 0x28) GXCopyTex of the EFB into the
  layer's `unk54`** — i.e. the coverage IS the EFB-rendered goo, copied back as R8 per layer
  (`efbTex->mImagePtr = unk54`, `mTexFmt = GX_CTF_R8`, one per `gpPollution->getJointModelNum()`).

## Root cause under ngx
Under ngx present Dolphin's EFB is empty, so the graffito-check GXCopyTex copies EMPTY → `unk54`
coverage = 0 → the TPollutionLayer planes sample empty coverage → invisible goo. And ngx doesn't
render the offscreen graffiti-group geometry either. So the coverage never exists.

## Tried / ruled out
- **Skip Dolphin's graffito-check copy (SUNBRIGHT_NGX_NOGRAFFITOCOPY, fmt 0x28) so the CPU coverage
  survives** → NO goo, even on a FRESH fastboot (not a stale save). Because non-map-9 `initTexImage`
  zeroes the coverage; there is no CPU coverage to preserve. The coverage genuinely comes from the
  graffito EFB render. (Experiment reverted — not committed.)
- The earlier `delfino-lighting-wash` "drawShineShadowVolume darkening" theory is a DIFFERENT effect
  (shadow-volume darkening), not this goo-coverage path.

## Native-ownership plan (the PC-engine way — DON'T replicate the EFB→R8 round-trip)
The coverage = "where the goo shapes are". Produce it in-engine:
1. **Capture the graffiti-group offscreen draws.** RE what the "落書きグループ" view objects draw
   (TPollutionObj / the goo decal geometry) and how — find the draw seam (likely J3DShape or a custom
   GX path ngx doesn't hook). Capture that geometry into ngx (a la the JPA/imm-geom captures).
2. **Render the goo coverage natively** into a per-layer R8 (or alpha) target by drawing the captured
   goo shapes — i.e. own the graffito pass as a real render-to-texture (NOT an EFB copy). Store it in
   the side buffer keyed by `unk54`'s EA so the pollution-layer material samples it (texture_for
   already serves g_efb_side; the fmt-40 R8 path would need the side-buffer store + leaving Dolphin's
   stomp off for that EA). This is per-epoch offscreen CONTENT (handoff task #1) specialized to the
   graffito pass.
3. Alternatively/additionally maintain coverage from `TPollutionManager::stamp/clean` directly (the
   PC-engine state-based approach) — but the INITIAL Manta-Storm goo placement must be found (where
   the beach-wide goo is stamped at episode start; not initTexImage for Sirena).

## Refinement (capture seam) — the goo IS the fastboot-verifiable consumer for task #1
ngx captures geometry via a tee on **J3DShape::draw (0x802e0390)** (ngx_j3d_shape.cpp ov_j3dshape_draw).
So BOTH are already captured as ngx batches: (a) the **TPollutionLayer plane** in the display epoch —
captured but TRANSPARENT because its coverage texture (unk54) is empty; (b) the **graffiti-group goo
shapes** in the offscreen "graffito" epoch — captured but they render as an invisible coverage MASK
(the final goo colour comes from the layer material modulating coverage, not from the mask shapes), so
rtfilter-off shows neither. ⇒ The fix is **per-epoch offscreen CONTENT (handoff task #1)** specialised
to the graffito pass:
  1. Render the graffito epoch's batches (the goo mask shapes) into an R8/alpha coverage target.
  2. Store it in `g_efb_side` keyed by the layer's coverage EA (the fmt-0x28 GXCopyTex dst, e.g.
     Sirena 80c72780) and make ngx SERVE that EA from the side buffer (the R8 path is currently
     Dolphin-only — add it to the served set + stop Dolphin stomping that EA).
  3. The display-epoch pollution-layer plane then samples real coverage → goo appears.
This gives task #1 a FASTBOOT-REACHABLE, VISIBLE consumer (the Sirena goo) — no hotel-mirror lobby
needed. Implement the generic per-epoch render in ngx_present.cpp render() (render a chosen epoch's
batches into a scratch target, read back, store keyed by that copy's dest EA), driven by the published
copy-event list (epoch→dest/dims/srcrect). Verify: fresh fastboot STAGE=6, goo appears on the beach.

## Reachability / tooling
- Fresh fastboot: SUNBRIGHT_STAGE=6 SCENARIO=0 (Sirena), settle PAST the ~35 s intro title-card wipe
  before judging. GX baseline (the real goo) = SUNBRIGHT_NGX_PRESENT=0 (Dolphin renders it).
- `gpPollution` is GMSE01 .sbss (J=0x8040A5A8, P=0x80405598 — USA addr differs; find via xref on a
  pollution fn, or r13=0x804141c0 SDA-relative). `getJointModelNum()`, `getLayer(i)->getUnk58()`.
- DBG_EFB shows the graffito copy: Sirena `GXCopyTex dst=80c72780 fmt=40 512x512` (the coverage EA).
  Note fmt-40 is NOT registered in g_efb_copy_addrs, so the present's [efb] CONSUMER detector won't
  flag a batch sampling it — add it if you need to confirm the pollution-layer plane samples it.
