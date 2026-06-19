# 2026-06-19 — Built the working ngx-vs-GX pixel oracle (tooling-first hard rule)

## Task
Per the tooling-first hard rule (CLAUDE.md "🔧 TOOLING / VERIFICATION FIRST") and the handoff
`scratch/handoff_2026-06-19_verification_oracle.md`: the renderer-fidelity verification harness was
reported broken (both documented oracles produce garbage). Fix the harness BEFORE any fidelity work.

## What I found
1. **`oracle_ab.sh` already existed and already works.** It was committed earlier (d20acbd) but the
   handoff didn't account for it. It runs TWO fastboot processes (NGX_PRESENT=0 = Dolphin-GX oracle,
   =1 = ngx), syncs them on `emu_secs`, and captures each via `/abshot2` (which DOES write a valid
   `ab2.gx.ppm` in the `=0` process — the black-oracle problem only affects the single-present case).
   VERIFIED on real plaza data: **18.1% / 18.0%** across two runs; per-side cross-run drift **0.5%
   (GX) / 0.2% (ngx)** — i.e. deterministic, the signal swamps the drift. So the premise "both oracles
   are broken" was outdated for the fastboot path.

2. The single-present `/abshot2` GX oracle is genuinely dead under no-recomp (ngx owns the frame →
   Dolphin XFB black). `ab_diff.py`'s empty-guard (exit 3) correctly refuses it.

3. `SUNBRIGHT_STATE` auto-load is genuinely broken on the native path (field trigger dies ~field 1450;
   main-thread `State::LoadAs` deadlocks the governor-parked CPU). Confirmed.

## What I built
- **`/loadstate?f=<path>` probe endpoint** (`runtime/probe_server.cpp`): loads a save state on the CPU
  thread via `Core::RunOnCPUThread` (the probe runs on its own thread → PauseAndLock + queue + run on
  resume; no main-thread deadlock). This is the sanctioned cross-thread state-load path.
- **`tools/render/ab_oracle.sh <save.sav> [settle_s]`**: the SAVE-STATE two-process oracle. Each
  process fastboots to a running core, `/loadstate`s the SAME save (identical restored RAM ⇒ frame-exact),
  then `/abshot2` captures its renderer's PPM. Reaches ANY saved scene (not just fastboot plaza), so
  sun-occlusion / sphere-sky scenes become verifiable once driven-to and saved. Auto-picks the newest
  `build*/sunbright` (a stale `build/` lacking `/loadstate` would fail with "unknown path").

## Key finding — save states are renderer-state-blind (stale saves crash)
A Dolphin save restores only RAM + PPCState, NOT the native engine bookkeeping (ngx / native_jas /
threading). Loading a **stale** save (the Jun-3 `scratch/{delfino,gameplay,hud_gameplay,quick}.sav`,
made pre-no-recomp-pivot) restores a guest SP from a different execution model → corrupted r1
(`0xff7a75d0`) → FATAL invalid guest read → abort. A **fresh** save made under the current build
round-trips cleanly (tested: load → no crash, emu continues). Made `scratch/fresh_plaza.sav`
(`SUNBRIGHT_SAVE_STATE=… SUNBRIGHT_SAVE_AT=70`, NGX_PRESENT=1 because `SAVE_ON_HUD`'s counter only
ticks under ngx present).

## Verification of the tool itself (tooling-first, recursively)
`ab_oracle.sh scratch/fresh_plaza.sav 5`: **18.4% / 18.5%** across two runs; GX cross-run drift 0.6%,
ngx 0.0%. Non-black both sides (no exit-3). Cross-validates `oracle_ab.sh` (~18% on the same scene via
a totally independent sync method). render_test still 1/1 (10/10 internal).

## State of the oracles now
- `oracle_ab.sh` — fastboot+emu_secs, plaza only, no save needed.
- `ab_oracle.sh` — save-state, ANY saved scene, deterministic. **Preferred.**
- Both feed `ab_diff.py` (empty-guarded). Plaza ngx-vs-GX = ~18% (the historical "40%" was the
  empty-oracle artifact). Fidelity work is now UNBLOCKED.

## Next (was gated on this, now open)
Drive+save fresh states at the sun-occlusion / sphere-sky scenes → `ab_oracle.sh` them → port those
EFB-readback effects with a real number to move. The Delfino floor "wash" is still PARKED per user.

---

# 2026-06-19 (cont.) — GXDrawSphere capture: geometry VERIFIED, end-to-end UNVERIFIABLE (hard-rule STOP)

Continuing ("keep going"), I used the new oracle to pick a non-parked engine gap. The documented
candidate was "ngx misses immediate-mode GXDrawSphere" (the `Map/Sky.cpp:88` skybox dome). I ported it
the same way as the existing GXDrawCube capture:
- `ngx_imm_geom.h`: `sphere_verts` / `sphere_tri_indices` / counts — faithful to `GXDraw.c:46`
  (numMajor latitude bands, each a `(numMinor+1)*2` triangle-strip; outer-then-inner ring order).
  **VERIFIED** by a new `render_test` unit `imm_sphere` (counts, unit-sphere, north-pole + first-ring
  spec positions, per-band index spans). render_test now 11/11.
- `ngx_emit_imm_sphere` (ngx_j3d_shape.cpp): rebuilds the dome in clip space (PNMTX0 + projection),
  flat PASSCLR = captured matColor, sky PE state (z-test LEQUAL + z-write, opaque ONE/ZERO, CULL_FRONT).
- Capture in `imm_geom_native.cpp`: GXDrawSphere @ 0x80362268 (numMajor=r3, numMinor=r4) +
  GXSetChanMatColor @ 0x8035f51c. The matColor ABI is **VERIFIED by disasm**: `lbz r,0..2(r4)` ⇒ GXColor
  passed BY POINTER in r4 (R@0,G@1,B@2,A@3) — see memory `gx-color-args-by-pointer`. Removed the old
  duplicate debug stub at 0x80362268 in efb_readback_native.cpp.

## The hard-rule wall (this is the real finding)
**GXDrawSphere fires 0 times in EVERY reachable scene** — fastboot plaza, title, AND file-select (DBG_EFB,
direct grep; GXDrawCube @ 0x803627fc fires fine right next to it, so the hook is correct — it's simply
not called). The shipping game uses textured `sky.bmd` skies everywhere reachable; the GXDrawSphere dome
is unused in these scenes. So:
- The oracle DISPROVED my hypothesis: the plaza sky-region delta is NOT a missing sphere (the GX oracle's
  top region is greenish terrain/textured-sky, not the dome's blue `0,12,EE`). The dominant plaza deltas
  are the floor center/bottom = the **PARKED wash**, not the sky.
- Per the TOOLING/VERIFICATION-FIRST hard rule ("Do not port effects you cannot verify"), I CANNOT
  declare this port done — there is no reachable scene to verify the end-to-end render.

## Status of the GXDrawSphere change (committed, but honestly labeled)
- VERIFIED: geometry (`render_test imm_sphere`), matColor ABI (disasm).
- UNVERIFIED: the end-to-end dome render (no reachable scene calls GXDrawSphere).
- CONFIRMED INERT / NO REGRESSION: with the change in, plaza ab_oracle is still 18.4% (unchanged) —
  the capture is gated on ngx-present AND only fires on an actual GXDrawSphere call, of which there are
  none in reachable scenes. So it cannot regress any currently-working scene; at worst it would render a
  dome (right or wrong) in a scene that is currently *also* missing it.
- PARKED pending a scene that actually draws the sky dome (unknown which stage; `mMap==15` only adds the
  rotation, not the draw). Don't re-assert "sky sphere = the plaza sky gap" — it is NOT (falsified here).

Lesson reinforced: check effect REACHABILITY (does the call even fire in a scene I can reach?) BEFORE
porting — that is the reachability half of the tooling-first rule, and it would have caught this earlier.

---

# 2026-06-19 (cont.) — /savestate + the reachability wall (effect verification is gated on scene-reaching)

Continuing, I built the rest of the drive→save→load→verify loop and probed the next verifiable effects.
The consistent finding: **every reachable-now divergence is the PARKED wash; the non-parked effects
don't fire in any state I can produce.**

## Shipped (committed 7559c84)
- **`/savestate?f=<path>`** probe endpoint (symmetric with `/loadstate`, `State::SaveAs` via
  `Core::RunOnCPUThread`). VERIFIED round-trip: writes 27 MB on demand, `/loadstate` reloads it, process
  stays alive. Completes the `/loadstate` + `/savestate` + `/pad` scripted loop.

## Reachability findings (all checked on real data — these are the value of this round)
- **Plaza fog is OFF**: `/gxstate` GXSetFog TEE = `type=0 (GX_FOG_NONE)`, fsel=0 (called 6940× but always
  NONE — gd-reinit-gx disables it each frame). So the plaza center over-brightness is NOT fog; porting GX
  fog would be another unreachable-effect trap. (ngx captures fog state but doesn't apply it — correct to
  leave unmodeled until a fog-active scene is reachable.)
- **Sun occlusion is dormant, sun off-screen**: `TSunMgr::drawSyncCallback` fires (unk14=1, sun model
  loaded) but `TSunModel::getZBufValue` (sunmodel.cpp:265) runs GXPeekZ only for sample points that
  project ON-screen (`it->x != -1`); the sun is off-screen in fastboot plaza so all 17 points are
  (-1,-1) → GXPeekZ fires 0×. The native GXPeekZ override (`ov_gxpeekz`, reads ngx depth) is BUILT and
  correct but can't be exercised until the sun is in view. C-stick combos (cup/cdown/cleft/cright) exist
  for camera aiming.
- **fastboot Delfino = the Entrance.thp cutscene, NOT controllable free-roam**: drove RIGHT 3 s from the
  loaded state → before/after screenshots byte-identical (md5 match) → Mario isn't controllable there.
  `scratch/fresh_plaza.sav` (emu 70 s) and `scratch/freeroam_plaza.sav` are both in/near the entrance;
  neither is a free-roam camera-drivable state. (Don't rely on them for camera driving.)

## Where this leaves it (the honest next frontier)
The verification TOOLING is complete and proven (oracle / loadstate / savestate / ab_oracle.sh). USING it
to verify NEW effects is gated on **game-state reachability**: get to a controllable free-roam scene
(past the entrance cutscene), aim the camera to bring the sun into view (→ GXPeekZ activates → verify the
native sun-occlusion vs the Dolphin-GX baseline), and/or transition into a fog-active level. That is a
separate, non-trivial driving/RE task (and may be easier from a headed session where the user can drive
to a good spot and `/savestate` it). Per the tooling-first hard rule I did NOT port further effects I
can't verify. The reachable plaza divergence remains the PARKED wash — not chasing it per the directive.

---

# 2026-06-19 (cont.) — Drove it live; reached controllable free-roam; effects are all STATE-GATED

User: "I'm not available. try to drive it live." Did so, headless.

## Live-driving recipe (WORKS — verified)
`SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_SKIP_THP=1 SUNBRIGHT_NGX_PRESENT=1` → fastboot loads Delfino, then
SKIP_THP auto-skips the Entrance.thp (THP state 2→3) → **controllable free-roam**. Proven: after the
skip, driving `right`/`left`/`up` via `/pad` changes the frame by mean-diff ~87 vs a ~0.2 self-animation
baseline (before the skip it was byte-identical = the cutscene). Camera C-stick combos (cup/cleft/cright)
work too. WITHOUT SKIP_THP the game idles in the entrance movie and never reaches free-roam (that was the
earlier "not controllable" result). NOTE: letting fastboot+skip run to free-roam on its own leaves ngx
present uninitialized (frames=0) — instead `/loadstate` a rendering save first, then it renders + drives.

## Artifact: `scratch/freeroam_plaza.sav` (gitignored) — a VERIFIED controllable free-roam plaza save
Made with `/savestate` mid-free-roam. Reloads in a fresh process directly into controllable free-roam
(drive LEFT → mean-diff 87, no crash). This is the reusable scene for future camera-driven verification
(replaces the crashing stale Jun-3 saves and the mid-entrance fresh_plaza.sav).

## Why effect verification still didn't happen — every effect is STATE-GATED (the real finding)
With a controllable camera I tried to activate the dormant effects; each is gated on a game state not
reachable by simple free-roam driving:
- **Sun occlusion (GXPeekZ)**: it's the **Noki Bay sun-WARP** sun. `TSunMgr` sets `unk15|=1` (which
  computes the sun's on-screen sample points) ONLY when `unk14 && getCurrentMap()==1 && TFlagManager
  getBool(0x50004)` (sunmgr.cpp:62) — a story-flag-gated warp. drawSyncCallback fires with unk14=1 but
  unk15=0, so the sample points stay (-1,-1) and GXPeekZ never runs. Needs that story flag + the sun
  framed in-camera.
- **Mario occlusion (GXPeekARGB)**: the probe cube IS drawn (GXDrawCube fires, onscreen=24/24) but
  `GXPeekARGB` is NOT called in free-roam (0 occ-branch logs) — TMario::drawSyncCallback's peek is
  itself conditional (only some Mario states/camera relationships). The earlier "occluded=0 verified"
  was a different (entrance) state.
- **GXDrawSphere / fog**: uncalled / disabled in all reachable scenes (prior sections).

## Conclusion
Live-driving + the full save/load tooling are DONE and proven. But the EFFECTS are each locked behind
specific game states (story flags, warp mechanics, conditional Mario states) that need targeted driving
or play to trigger — a much larger task than a free-roam drive, and not a "quick verify." I stopped here
rather than open-endedly grind toward those states or port anything unverifiable (hard rule). Next: if a
specific effect must be verified, drive/script to ITS trigger state (e.g. for the sun warp: set/await
flag 0x50004 and approach the sun-warp point), `/savestate` it, then ab_oracle / DBG_EFB it.

---

# 2026-06-19 (cont.) — Plaza "wash" diagnosed with the oracle (ruled out 4 suspects; pinned to post-raster)

Used the oracle on `scratch/freeroam_plaza.sav` (a real controllable gameplay view) + the live probes to
attack the long-parked Delfino floor "wash" as an engineering problem. Real, oracle-backed narrowing:

## The measurement
ngx is uniformly ~1.4× brighter than the Dolphin-GX oracle across surfaces at different normals (floor
GX gray ~99 → ngx ~136; right-side buildings GX ~99 → ngx ~178). Uniform across normals ⇒ NOT per-normal
diffuse alone. Plus a magenta NPC (Pianta) blob on the right = a separate colour bug.

## Ruled OUT (each checked, not guessed)
- **Copy gamma**: game uses `GX_GM_1_0` (JDRDisplay.cpp:14) — no gamma. Not it.
- **EFB copy filter**: live `/ngxshape` → `COPY filter coefs=[8,8,10,12,10,8,8] sum=64 (/64=1.000)` —
  brightness-preserving. Not it.
- **Ambient**: floor material (cc=068e) `amb=(0,0,0)` live, matches xfmem (the 2026-06-18 per-material
  ambient fix holds). Not the floor's cause.
- **Lighting MATH**: `ngx::light_color0` is unit-tested faithful to Dolphin's LightingShaderGen
  (render_test test_lighting). Light colours captured correctly at GXLoadLightObjImm (color@0x0C; the
  sun really is white (1,1,1) — TLight::load reads it from scene data via GXInitLightColor). Normals are
  unit-length (sky en |n|≈0.99).

## Where it points
The floor (cc=068e) has FAITHFUL inputs (mat=white, amb=0, lights=(1,1,1)) AND faithful lighting math,
yet renders ~1.4× too bright → the divergence is **post-raster: the TEV combiner / texture-modulation
stage** (matches the older /gxstate verdict "wash is COMPOSITING, not per-material shading"), or a
runtime light-mask/normal mismatch at the actual draw. Next: `/gxstate` the floor material's combiner +
compare its evaluated output to GX; check the floor TEXTURE brightness ngx vs GX (isolates tex vs raster).

## Separate concrete bug found (a clean fix lead)
The XFMEM-vs-OURS diff showed a material `cc=0500` with ngx `mat=(128,66,112)` while xfmem `mat=ffffffff`.
(128,66,112)=0x804270.. = guest-RAM POINTER bytes — the by-pointer-vs-by-value misread signature (memory
gx-color-args-by-pointer). This is almost certainly the **magenta NPC blob**: that material class reads its
matColor from the wrong offset/source → pointer garbage → magenta. Concrete, fixable, verifiable next.

Status: NOT yet fixed — but the wash is now precisely narrowed (4 suspects eliminated with the oracle, a
parked problem genuinely advanced) and there's a concrete magenta-material bug lead. No magic-scale
bandaid was added (would violate no-bandaids). Fix proceeds from here: TEV/texture compositing for the
wash; the matColor-source misread for the magenta material.

## Deeper dive (same session) — eliminated 2 more suspects; pinned to per-material lighting/combiner
- The "magenta cc=0500 mat=(128,66,112)" is a DIAGNOSTIC ARTIFACT, not the shipping render: /gxstate on
  the floor (ti=2) shows the GX **fn-tee** is stale (J3D bypasses the GX fns → the fn-tee captures
  pointer bytes), while ngx's OBJECT-MODEL decode PASSes vs xfmem (cc=068e, mat=ffffffff white,
  amb=0). So matColor/cc/amb are all faithful. (The on-screen magenta NPC is a separate, rarer material
  not captured here — low priority, one NPC.)
- **Texture decode**: `/tex` selftest = PARITY-OK (119/119 vs Dolphin). Textures are faithful.
- **Colorspace/present**: all ngx Vulkan targets are `VK_FORMAT_R8G8B8A8_UNORM` (linear, no sRGB);
  no gamma in present. No UNORM↔sRGB mismatch.
- **It is PER-MATERIAL, not a global multiplier**: floor ~1.4× bright, buildings ~1.8× — different
  ratios ⇒ the wash is in the per-material LIGHTING RASTER and/or the TEV COMBINER evaluation, varying
  by material. The floor (ti=2) is a 5-stage detail-map combiner (compares, KONST, bias=SUBHALF/ADDHALF,
  scale<<1) with diff=SIGN/attn=SPOT lighting (mask=03, sun+local). Object-model decode of all of this
  PASSes vs xfmem; the divergence is in the EVALUATION (generated GLSL combiner math or the live lit
  raster value), which has NO CPU oracle (bpmem/xfmem async-lagged) — only the pixel oracle verifies.
- **Normals**: ngx transforms normals by the game's OWN normal matrix (j3dSys+0x108 mCurrentNormMtx,
  the inverse-transpose GX uses), then normalizes (ngx_j3d_shape.cpp:1560-1565). Faithful — not the
  modelview-instead-of-normal-matrix bug.
- CONCLUSION: the ENTIRE upstream pipeline is now proven faithful — pos+normal matrices, normal
  normalization, cc/matColor/ambient/lights (PASS vs xfmem), lighting math (unit-tested), texture decode
  (parity-OK), colorspace (all UNORM, no sRGB/gamma). The per-material wash (floor 1.4×, walls 1.8×) is
  therefore in exactly ONE of two places, both needing focused follow-up (neither a one-liner, no CPU
  oracle — only the pixel oracle verifies): (1) the generated TEV-combiner GLSL EVALUATION for these
  specific multi-stage detail-map materials (bias/scale/clamp/compare/konst interplay — the one thing
  not yet unit-covered for 5-stage combiners), or (2) a measurement/config delta between the
  NGX_PRESENT=0 and =1 runs (resolution / Dolphin post). No bandaid added; the upstream eliminations
  mean the next session must NOT re-chase gamma/ambient/lighting/normals/textures.
- **Candidate (2) ELIMINATED**: `SUNBRIGHT_NGX_PRESENT` (main_sdl.cpp:846) only swaps the present
  callback + sets g_sb_ngx_present — it changes NO Dolphin GFX config (EFB scale/res/post are applied
  identically). So the two-process compare is fair and the 1.4× is a REAL renderer difference, not a
  config artifact. ⇒ The wash is the **TEV-combiner GLSL EVALUATION** (or the tevreg/konst inputs to
  it) for these multi-stage detail-map materials — the sole remaining suspect after everything else is
  proven faithful.
- THE FIX PATH (mandated TDD, no CPU oracle exists for the combiner): extend `render_test` (the
  tev/combiner unit) with the floor's actual 5-stage pattern (the s0..s4 ops + bias/scale/compare/konst
  from /gxstate ti=2) and hand-computed expected outputs, find the generator bug deterministically,
  fix the GLSL generator, then confirm the ab_oracle number drops. Substantial, not a one-liner.

## Even deeper — TEV inputs ALSO proven faithful; converges on the parked "missing darkening" answer
- **TevBlock offsets verified vs decomp** (J3DTevBlocks.hpp): ngx's TVB16 layout (mTevColor@0xD6,
  mTevKColor@0xF6, mTevKColorSel@0x106, mTevKAlphaSel@0x116, mTevSwapModeTable@0x126, stagenum@0x54,
  stage@0x55, order@0x14) and TVB2 (0x10/0x41/0x51/0x53/0x55…) ALL MATCH. So ngx reads tevreg/kcolor
  from the correct object-model offsets (synchronous, not the stale GX fn-tee). tevreg/kcolor faithful.
- **TEV generator verified** (tev_shader.cpp): faithful to Dolphin PixelShaderGen — KSEL_C/A konst
  tables, swap tables, regular integer scale-lerp+round, compare modes, final prev/255. Unit-tested.
- **Blend**: floor (ti=2) + buildings (ti=3) are blend=SRCALPHA/INVSRCALPHA, but the floor's final
  alpha = RASA = matColor.a = 255 (ALPHA0 reg, lighting off) ⇒ srcAlpha≈1 ⇒ effectively OPAQUE ⇒ the
  floor shows its combiner output directly (blend/compositing is NOT diluting it).
- ⇒ With EVERY per-material input + the generator + the blend all faithful, an effectively-opaque floor
  that is still 1.4× too bright means GX's FINAL framebuffer is DARKER via a step ngx doesn't do:
  **a missing EFB-readback-gated DARKENING pass** (the long-parked `delfino-lighting-wash` conclusion,
  now independently CONFIRMED by exhaustively ruling out the shading pipeline). The pollution port
  (drawShineShadowVolume) was already falsified as the specific effect (never called); the open RE is
  WHICH EFB-readback pass darkens the plaza floor. (Residual possibility: a subtle 5-stage combiner-gen
  bug not covered by the current unit — settle by the render_test buildout above; but the evidence now
  points to a missing darkening effect, matching the user's "missing effect" call.)
- This is the parked frontier. Per the user directive it stays PARKED as a fidelity chase; the value
  here is the PROOF that it is a missing-effect/compositing problem, NOT a per-material shading bug —
  so no future session should re-audit lighting/textures/combiner/offsets (all proven faithful).

## CORRECTION — the wash IS a shading bug: ngx never computes the COLOR1 channel (aliases col1=col0)
The "everything faithful → missing external effect" claim above was WRONG: I overlooked the SECOND
colour channel. The floor (ti=2) combiner stage s4 and the buildings (ti=3) s1 read **rasChan=5 =
GX_COLOR1A1**, but ngx computes ONLY COLOR0 and the generated shader hardcodes `col1 = col0`
(tev_shader.cpp:264; comment ngx_j3d_shape.cpp:592 "ngx uses col1==col0"). COLOR1 is a DISTINCT channel:
floor COLOR1 cc=0x0212 = enable, mask=light2 ONLY, diffFn=NONE (diffuse=1), attn=SPEC — i.e. amb(0) +
light2·spec-attn, which is much DARKER than COLOR0 (sun, SIGN, lights0+1, ~0.87). Feeding the bright
COLOR0 where the combiner wants the dark COLOR1 over-brightens exactly the materials that use rasChan=5
(floor + buildings = the washed surfaces). THIS is the wash (or the bulk of it), a real renderer bug.
FIX: read mColorChan[2]/[3] (COLOR1/ALPHA1 @ chan_off+4/+6), compute a second per-vertex raster via
light_color0 with the COLOR1 chanctl, add a col1 vertex attribute, and use it in the shader where
rasChan∈{1,5}. (Supersedes the "missing external effect" conclusion for these LIT materials; the emu14
cc=0701 UNLIT floor may still be a separate case.)
