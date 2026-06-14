# interp60 — EFB-feedback + effect-interpolation handoff (2026-06-14) — READ FIRST

Continues `docs/interp60_handoff.md`. **Frontier 1 (cadence) is DONE; this doc is the OPEN
Frontier 2 (effects wrong under interpolation).** Read this before touching the EFB/effect code —
it lists every dead end so you don't repeat the ~8 wrong hypotheses this session burned.

---

## ✅ UPDATE (2026-06-14, next session): UNIFIED REPLAY — own BOTH presents through one pipeline
This is the fix the previous session's "Next idea if resuming" pointed at, and it matches the user
directive (own the render, not tweak knobs). **Both the real present AND the in-between present are now
produced by the SAME render-only replay** (`runtime/overrides/interp_redraw.cpp`, the replay branch):

- **REAL present** = `gxs_replay_frame` at **alpha 1.0** (== frame N exactly) into a freshly-cleared
  EFB → copy → orig XFB → present.
- **IN-BETWEEN present** = `gxs_replay_frame` at **g_i60.alpha** (lerp N-1→N) into a freshly-cleared
  EFB → copy → alt XFB → present.

The game's live 30 Hz render still runs (it builds the captured GX stream) but **its EFB output is
discarded** — we `sb_clear_efb()` and re-render both presented frames ourselves. Each replay re-runs
the engine's OWN EFB-copy/screen-texture commands, so each present samples its OWN freshly-copied
screen texture. **Why this fixes Frontier 2 by construction:** previously the real present was the
game's LIVE render, which sampled the screen texture left by the PREVIOUS in-between (N-1/2) → water/
mirror/EFB feedback lagged a half-step on real frames while the in-between (a replay) looked right —
two DIFFERENT pipelines that disagreed. Now they are ONE pipeline and cannot disagree (the user's
"there should be only one outcome").

- Built clean, runs headless: redraws climb, motion-interp full coverage (hits≫misses), **0 new
  gameplay doublings + 0 new gameplay skips** over a 14s walk (Frontier 1 held). Mechanism for the
  pairing map at two alphas: `interp60_xfmap_build` ONCE (correct freshly-spawned gating off LAST
  frame's registry) + `interp60_xfmap_set_alpha` per replay (the map is alpha-independent base<->base
  pairing). At alpha 1.0 the lerp collapses to N for every model, so the real present is frame N
  regardless of pairing.
- A/B off-switch: `SUNBRIGHT_NO_UNIFIED_REPLAY=1` reverts to the old live-real-frame path (the
  lagged one) for comparison. Delete that fallback once verified headed.
- **NEEDS USER HEADED VERIFICATION** of the water/tower reflection (headless capture can't force the
  motion-dependent artifact; per "verification is broken" below). The old EFB-redirect/owned-texture
  machinery (efb_native.cpp, m_sb_efb_own, sb_efb_reset_binds) is now only used by the A/B fallback
  and is a candidate for deletion after confirmation.
- The DEAD ENDS list below remains valid history; the unified-replay approach sidesteps all of them
  by not trying to patch the in-between or the real frame separately.

---

## ★ ROOT CAUSE PINNED (2026-06-14) — water reflection SLIDES, interp60-only
User ground truth: the jitter is **interp60-only** (plain 30fps water is fine) and is the
**reflection sliding/swimming relative to the (correctly interpolated) surface**. Mechanism, fully
RE'd (docs/re_notes/water_refraction_projection.md):
- `TModelWaterManager::drawRefracAndSpec` (0x8027c12c) draws the refraction with **PNMTX0 = IDENTITY**
  and a **view-less texmtx** (slot 0x1e = `C_MTXLightPerspective(fovy,aspect)`, no rotation/translation).
  The screen-UV comes from the quad's **eye-space vertex positions** (`unk5D30`, a TDLTexQuad), which
  the game built at tick N from `gfx+0xB4` and **baked into the GX stream as raw vertex data**.
- The raw-GX replay (unified replay / `gxs_replay_frame`) re-emits those **tick-N** quad verts
  verbatim. The indexed-matrix interp seam (LoadIndexedXF, array 12) only substitutes pos *matrices* —
  it never touches raw vertex data, and the refraction uses identity PNMTX anyway. So the refraction
  quad's projection stays at **N** while the screen texture it samples is re-rendered at **N½** →
  reflection swims. Real frames are consistent (both N) — unified replay fixed those.
- **STRUCTURAL WALL:** `water_native.cpp` owns the projection by hooking the GUEST functions
  (drawRefracAndSpec / C_MTXLightPerspective). The raw-GX replay makes **NO guest calls**, so those
  overrides never fire on the in-between — nothing re-derives the water projection per-field.
- **Why blanket direct-XF interp failed:** it interpolated the small-delta HUD ortho matrices (mangled
  the HUD) and the >8000 cut-guard rejected the large-entry water projection matrix (so it never even
  interpolated the thing that mattered). REVERTED.
- **BUILT (2026-06-14, commit d26ac3a) — re-derive the water via the guest path at N½** (user-chosen
  direction): after the in-between raw replay, re-issue `TModelWaterManager::perform(0x8027beb0)` with
  flags `&4|&0x80` through the guest path, passing a fabricated gfx whose `mViewMtx` (+0xB4) =
  `lerp(prevView,curView,alpha)` of the j3dSys view (0x804045DC). `&4` (calcVMAll) rebuilds the
  eye-space quad at the N½ camera; `&0x80` draws the refraction sampling the N½ screen texture (the
  replay already copied it correctly). Caches the live water manager from real-field perform calls
  (reset each frame in mardir_direct). Off-switch `SUNBRIGHT_NO_WATER_REISSUE`. Verified headless:
  stable, no crash, water renders clean, no gross doubling, cadence unchanged. **NEEDS USER HEADED
  VERIFY of reflection tracking during camera motion.** KNOWN RISK: this overdraws the replay's frozen
  tick-N water (double-draw); if doubling is visible, suppress the frozen water in the raw replay
  (skip its draw via an OpcodeDecoder/primitive seam keyed off the texmtx-0x1e marker).
- **Alternative candidate (superseded by the above unless it double-draws):** on the in-between replay, detect the water refraction draw
  by its texmtx-slot-0x1e load marker and substitute PNMTX0 = view(N½)·view(N)⁻¹ (eye-space
  reprojection of the tick-N quad to the interpolated camera) so quad + screen-texture agree. Targeted
  (water marker only, won't touch HUD). Needs the eye-space-delta math + HEADED verification.
  Alternative per water_refraction_projection.md §6: re-issue the water draw via the guest path on the
  in-between with gfx+0xB4 set to N½ (overrides fire) — hybrid, heavier.

## ⛔ USER DIRECTIVE (most important — re-read every time)
> "stop tweaking knobs, own more of the code, less dolphin, less emulation, more native code,
> like the interp you made that lives over the renderer."

The user is right and has said it repeatedly. **Do NOT chase Dolphin texture-cache / config knobs.**
The motion-interp (render-only replay that lives over the renderer) is the model: OWN the rendering
of these effects natively. Also: **the user is the ground-truth verifier** — I cannot see the
artifacts headless (see "Verification is broken" below). Capture frames, let the user judge.

---

## STATE OF THE TWO PROBLEM CLASSES

The scene interpolates in three layers; only one is correct:

| Class | Mechanism | Status |
|---|---|---|
| **Geometry** (world/Mario/NPCs) | indexed position matrices (CP array 12), interpolated in `interp_capture.cpp` | ✅ correct, 60fps |
| **EFB feedback** (water reflection/refraction, mirror, graffiti) | EFB→texture readback sampled by the water shader | ❌ **real frames wrong, in-between correct** — UNRESOLVED |
| **Direct-transform effects** (banners, smoke, projected shadows/decals) | **direct XF register writes** (modelview/texgen matrices), NOT indexed | ❌ jitter on camera rotation — NOT STARTED |

### The concrete EFB symptom (user's tower-over-water images, the clearest evidence)
- **REAL frame:** the tower's water reflection is **doubled / offset** (a second tower in the water).
- **IN-BETWEEN frame:** clean — reflection sits correctly under the tower.
- Same in `skip` and `own` modes. Mario "ghosts" in water the same way.
- **The real and in-between DISAGREE — that is the core problem.** User: "Idk which one is right
  but there should be only one outcome."

---

## WHY real ≠ in-between (the key structural insight)
The two frames are produced by **two different code paths**:
- **Real frame** = the **GAME's** direct render (`mDirector->direct` → normal engine→Dolphin path).
  This is where the bug lives.
- **In-between** = **my replay** (`gxs_replay_frame`, lives over the renderer). Confirmed correct.

They will always diverge because they are not the same pipeline. The user's directive points at the
fix: **render/own BOTH frames through the same path**, OR fix whatever the game-render path does that
the replay path doesn't.

### Leading unproven hypothesis (where I'd resume): un-cleared EFB residue
The interp does **two** GXCopyDisp presents per game frame (real + in-between). The in-between is
clean because the real present's copy clears the EFB *before* the replay draws. But **nothing clears
the EFB before the GAME's next real-frame render**, so the game draws the next real frame on top of
the previous in-between's geometry residue → the water's screen-copy captures both → doubled
reflection. This fits ALL the evidence (real wrong / in-between clean / same in skip+own).
- **Tried:** `sb_clear_efb()` (BPFunctions.cpp) at end of the interp frame, opt-in
  `SUNBRIGHT_EFB_CLEAR=1`. It DOES materially change the real frames (residue removed), renders fine
  — but the user said **"mario is still the same."** So either the clear is insufficient (Mario's
  reflection is a different mechanism than the tower residue), or it needs to also clear depth/other
  state, or the residue theory is incomplete. **Unverified — could not A/B headless.**
- **Next idea if resuming this:** present the REAL frame via a replay@alpha=1.0 too (own both frames
  through the identical replay path → one outcome by construction). The in-between path is correct,
  so replay@1.0 would be a correct real frame. Needs EFB-clear-before-each-replay handled.

---

## DEAD ENDS — do NOT repeat these (all tried + disproven this session)
1. **Address redirect (copy EFB→`alt=orig^0x400000`, sample alt).** Crashes — even `copy_to_ram=false`
   still `UninitializeEFBMemory()`-writes the dest RAM, and `alt` aliases the J3D draw buffer → wild
   read in `J3DDrawBuffer::drawHead` (ea=0x10). Also the alt RAM hash → sampled garbage.
2. **Owned textures (`m_sb_efb_own`) for the in-between.** Works (in-between correct, `hit=5586`) but
   does NOT fix real frames. **Instrumented + PROVEN:** `REAL-frame reused OWNED tex=0` (no leak),
   `fallthrough-RAM=0` (no clobber). The owned path never touches the real frame.
3. **Bind-reuse leak / `SbResetBinds()`.** Hypothesis: `Load()` reuses the last-bound texture
   (TMEM-cached/hash-unchanged) so the in-between's owned tex leaks into the real frame. Added the
   reset (`bind-resets=1390`). **Disproven** by the leak counter = 0 AND user saw no change.
4. **Skip the in-between EFB copy (b1, current default).** "Fixes" distant-object lag by **deleting**
   the in-between's effect (symptom removal, not a fix — user's words). Does NOT fix Mario.
5. **EFB clear (above).** Didn't fix Mario. (May still fix the tower residue — unconfirmed.)

The recurring lesson: **the bug is in the REAL-frame (game) render path, not the in-between path.**
Every fix I aimed at the in-between was aimed at the wrong frame.

---

## ⚠ VERIFICATION IS BROKEN HEADLESS — fix this FIRST next session
I could not verify any EFB fix because:
- The artifacts are subtle/scene-specific (water reflection) — invisible in most stills.
- **Captures are NOT frame-deterministic:** `/verify` does a synchronous GPU readback per present
  that perturbs timing differently each run, so "hold right from boot" lands at different Mario
  positions per run → A/B by frame number compares different scenes (RMSE diffs were meaningless).

**Build a deterministic capture FIRST** (e.g. drive a fixed number of GAME FRAMES not wall-ms; or
capture via the frame-dump path without the readback perturbation; or step the game deterministically).
Without frame-aligned A/B you're guessing. The user offered the reliable path: **capture frames to
disk, the user labels good/bad** (`scratch/verify/`, even=real or odd=real per the capture's phase —
check the filenames). `SendUserFile` works but the user prefers files left on disk.

---

## TOOLING / FLAGS (all built this session)
- `/interp60` probe: `EFB-OWNED` line (copies/hit/miss/fallthrough-RAM), `EFB-LEAK` line
  (bind-resets / real-frame-reused-owned), `OWN-PRESENT`, `CADENCE`, XF array histogram.
- `/verify?n=K` → dumps K presents full-res to `scratch/verify/s<ts>/fNNN_<real|btwn>_<addr>.ppm`.
- Flags (all default to the safe skip baseline):
  - `SUNBRIGHT_EFB_OWN=1` — per-field owned EFB textures (in-between correct, real still lags).
  - `SUNBRIGHT_EFB_CLEAR=1` — clear EFB each interp frame (residue fix; didn't fix Mario).
  - `SUNBRIGHT_NO_EFB_REDIRECT=1` — disable in-between EFB handling entirely.
  - `SUNBRIGHT_EFB_NORESET=1` / `SUNBRIGHT_EFB_NOCONSUMER=1` — A/B bisection knobs.
- Fork seams added: `m_sb_efb_own` + owned branch in `CopyRenderTargetToTexture`/`GetTexture`
  (TextureCacheBase), `sb_clear_efb` (BPFunctions), the owned-present (`sb_present_xfb`, Present.cpp),
  the LoadIndexedXF interp hook (`sb_slot_xf_indexed`, XFStructs).

---

## DIRECT-TRANSFORM INTERP — STARTED (2026-06-14): direct LoadXFReg matrix interpolation
The water still jittered after unified replay. RE'd cause: the water screen-space refraction
**projection/texgen matrix is a DIRECT `LoadXFReg` write** to XF matrix memory, which the indexed
(array-12) interp hook never sees — so on the in-between the surface verts interpolate (indexed) while
the screen-space warp matrix stays at tick N = the "swim"/jitter (efb_dynamic_texture_chain.md §3's
non-linear warning). Same class as banners/smoke/projected shadows (all direct LoadXFReg).

**Native fix (commit pending), behind `SUNBRIGHT_INTERP60_DIRECTXF=1` (default off until verified):**
a new fork seam `sb_slot_xf_reg` in `XFStructs.cpp` `LoadXFReg` (the XF-mem branch), mirroring the
indexed seam. Runtime side in `interp_capture.cpp`:
- REAL replay (`interp60_dxf_begin(1)`): RECORD every direct matrix-memory write, keyed by
  (xf-mem address, k-th occurrence within the frame). Renders N as-is.
- IN-BETWEEN replay (`interp60_dxf_begin(2)`): substitute `lerp(N-1, N)` for each write at the same
  (addr, occ). A per-element **magnitude guard (>8000 → render at N)** rejects a mispaired reused slot
  (draw-order shift) so a bad pair can't explode geometry.
- `interp60_dxf_rotate()` after the in-between: N becomes N-1 for next frame.

Verified headless: enabled, **96–98% coverage** (interp≈554k of 574k writes over a walk+camera-rotate),
miss/cut low (cut ≈1.9%, all safely rendered at N), **no crash, no geometry explosion, cadence +
motion-interp unchanged**. `/interp60` "DIRECT-XF" line reports recorded/interp/miss/cut.
**NEEDS USER HEADED VERIFY:** does it smooth the water/banner jitter? If yes, flip default on; the
pairing-by-(addr,occ) may need scoping to texgen/post-matrix ranges if any effect mispairs visibly.

## DIRECT-TRANSFORM JITTER (banners/smoke/shadows) — original scoping below (now addressed by the seam above)
Confirmed via the `/interp60` XF array histogram: the scene only does **indexed** loads for arrays
**12 (position)** and **13 (normal)**. So banners, smoke particles, and projected-texture
shadows/decals set their transforms via **direct `LoadXFReg` writes** (modelview + texgen-projection
matrices), which the indexed-load interp hook never sees → they render at frame N over N−½ geometry →
jitter on camera rotation.
- **Fix:** a new seam in `XFStructs.cpp` `LoadXFReg` (alongside the existing `LoadIndexedXF` one) that
  interpolates the direct matrix writes during the replay.
- **The hard part — pairing:** direct writes have no object identity (unlike the J3D model registry
  used for array 12). User's guidance: **particles aren't impossible — the game tracks its own
  particle objects (JPA); pair by the game's particle/object identity, not draw order.**
- User's priority order was: **EFB first, then banners, then particles.**

---

## WHAT'S COMMITTED (Frontier 1, solid) vs WIP
- **Committed + pushed (parent `main` + fork branch `sunbright`):** owned-present clean cadence
  (Frontier 1, 13.2%→0% gameplay doublings), boot-skip revert, EFB skip-as-default, owned-EFB
  infrastructure behind `SUNBRIGHT_EFB_OWN`.
- **This handoff commit:** bind-reset + leak diagnostics + `sb_clear_efb` (all behind flags, default
  = skip baseline). Default build behavior is unchanged/safe.

## HOW TO REPRODUCE
```
SUNBRIGHT_HEADLESS=1 SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_INTERP60=1 SUNBRIGHT_PROBE=1 ./build/sunbright
curl '127.0.0.1:17654/pad?do=right&ms=12000'        # hold right from the plaza
curl '127.0.0.1:17654/verify?n=100'                 # dump 100 presents to scratch/verify/
```
Launch gotcha: the background shell sometimes loses cwd → no log file; kill in a SEPARATE step then
launch with `run_in_background`. NEVER overlap two instances (a zombie on port 17654 confounds reads).
