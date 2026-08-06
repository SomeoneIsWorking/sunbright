# 60fps and EFFECTS — the standing directives, the RE'd root cause, and the dead ends

**Provenance.** Extracted from `docs/interp60_handoff.md` and `docs/interp60_efb_handoff.md` (June
2026), which are deleted. Those were session handoff briefs — an anti-pattern this project's own
instructions ban — and every file they name (`interp_redraw.cpp`, `water_native.cpp`,
`interp_capture.cpp`, `efb_native.cpp`) belongs to the retired Dolphin-substrate recomp era and no
longer exists. **The findings below do not depend on that code.** They are about what the GAME does,
and they are the reason "port effects to be 60fps compatible" is a specific piece of work rather
than an open-ended one.

Read this before touching effect interpolation. It lists the dead ends so they are not repeated.

---

## ⛔ STANDING USER DIRECTIVES

**No skips, ever.** Every gameplay frame gets a real present AND an in-between present — a clean
`R,B,R,B,…` stream at 60 fps, in every scene, state and load condition. Not a tolerable edge case,
not a transition exception, not a perf fallback. (Boot/logo frames are explicitly exempt; the
directive is about gameplay. Clarified by the user 2026-06-14 after an earlier change that removed
the boot skips was reverted at their request.)

**Own the code; do not tweak knobs.** *"stop tweaking knobs, own more of the code, less dolphin,
less emulation, more native code, like the interp you made that lives over the renderer."* The
interpolation that lives over the renderer is the model: own the rendering of these effects
natively.

**The user is the ground-truth verifier for motion artifacts.** A reflection that swims, a trail
that doubles, a shimmer — these are motion-dependent and a headless capture cannot force them.
Capture frames, let the user judge. Do not report an effect fixed on the strength of a still.

---

## ✅ RE'd ROOT CAUSE — why the water reflection SLIDES under interpolation

Full RE in `docs/re_notes/water_refraction_projection.md`. The mechanism, which is a property of the
game and therefore still current:

`TModelWaterManager::drawRefracAndSpec` (US `0x8027c12c`) draws the refraction with **PNMTX0 =
IDENTITY** and a **view-less texture matrix** (slot `0x1e`, `C_MTXLightPerspective(fovy, aspect)` —
no rotation, no translation). The screen-space UV therefore comes from the quad's **eye-space vertex
positions**, which the game builds at tick N from `gfx+0xB4` and **bakes into the GX stream as raw
vertex data**.

So on an in-between frame: the replayed stream re-emits those **tick-N** quad vertices verbatim,
while the screen texture the quad samples has been re-rendered at **N½**. Quad at N, texture at N½ →
the reflection swims relative to the surface it sits on. Real frames are self-consistent (both at
N), which is why the artifact is interpolation-only and why plain 30 fps water is fine.

**Interpolating matrices cannot fix this.** A matrix-substitution seam touches position matrices; it
never touches raw vertex data, and this draw uses an identity PNMTX anyway.

### The structural wall

A raw-GX replay makes **no guest calls**. Any fix that works by hooking a guest function
(`drawRefracAndSpec`, `C_MTXLightPerspective`) therefore never fires on the in-between frame. An
effect can only be corrected on a presentation frame by something that is *invoked on presentation
frames* — which is exactly what `sb::frame_interp::add_interpolation_callback` is for, and why the
unified API has it.

### The shape of the fix that was built and worked

Re-issue the effect's own `perform` on the in-between with a **fabricated `TGraphics` whose view
matrix (`+0xB4`) is `lerp(prevView, curView, alpha)`**. For water: `TModelWaterManager::perform`
(`0x8027beb0`) with flags `&4 | &0x80` — `&4` (calcVMAll) rebuilds the eye-space quad at the N½
camera, `&0x80` draws the refraction sampling the N½ screen texture. Verified stable and clean
headless at the time; never headed-verified for reflection tracking during camera motion.

**Known risk, unresolved:** this overdraws the replay's frozen tick-N water. If doubling is visible,
the frozen water draw must be suppressed in the replay — it is identifiable by its texmtx-slot-`0x1e`
load marker.

---

## The three layers, and which one is which problem

| class | mechanism | state |
|---|---|---|
| **geometry** (world, Mario, NPCs) | indexed position matrices (CP array 12) | interpolates correctly |
| **EFB feedback** (water reflection/refraction, mirror, graffiti) | EFB→texture readback sampled by the effect's shader | the real and in-between frames DISAGREE — see below |
| **direct-transform effects** (banners, smoke, projected shadows/decals) | **direct XF register writes**, not indexed matrices | jitter on camera rotation; never started |

The third row is worth reading twice: those effects write their transforms straight to XF registers
rather than going through the indexed-matrix array, so **every mechanism this project has built for
pairing and interpolating matrices misses them by construction.**

### "There should be only one outcome"

The user's framing of the EFB problem, and it is the correct one. The real frame and the in-between
frame were produced by two different code paths — the game's own render versus a replay — and two
different pipelines will always be able to disagree. The fix direction is to produce **both** through
the same path, not to patch the difference.

---

## DEAD ENDS — tried and disproven, do not repeat

- **Blanket direct-XF interpolation.** Mangled the HUD (it interpolated the small-delta ortho
  matrices) and the >8000-entry cut-guard rejected the large water projection matrix, so it never
  interpolated the one thing it was added for. Reverted.
- **EFB address redirect** (copy EFB→`alt = orig ^ 0x400000`, sample alt). Crashes.
- **Clearing the EFB before the game's real-frame render** (`SUNBRIGHT_EFB_CLEAR=1`). It does
  materially change the real frames and it renders fine — and the user reported the artifact
  unchanged. So either the clear is insufficient, or it must also clear depth/other state, or the
  residue theory is wrong. Unresolved, and it could not be A/B'd headless.
- **Chasing Dolphin texture-cache / config knobs.** Explicitly ruled out by directive, and the
  Dolphin substrate is retired regardless.

---

## How this maps onto the current code

`sms-recomp/frame_interp/` (see `docs/60fps/README.md` for the map):

- `add_interpolation_callback(cb, user)` is the seam this document argues for. It is live, it is
  dispatched from `aurora_replay_midpoint()` — genuinely between a tick's two presents, and
  verified to run BEFORE the in-between frame is built (`extern/aurora/lib/aurora.cpp:876`, ahead of
  `begin_frame()` and `install_replay_snapshot()`) — and **nothing registers on it yet**. The
  per-run report says so in those words.

  **But a callback cannot draw into that frame**, and this changes what the fix has to be. The
  in-between image is a snapshot of the tick's recorded passes; `install_replay_snapshot()` discards
  the pass `begin_frame()` created and substitutes the snapshot's, so GX emitted from a callback
  lands in the NEXT tick's stream. The "re-issue `TModelWaterManager::perform` with a fabricated
  view" fix above was written for the retired era, where the in-between frame was produced by a
  replay that the host drove and could interleave guest calls with. It does not transfer as written.

### The water fix IS ALREADY IMPLEMENTED — do not write it again

This section previously prescribed patching the recorded stream to substitute
`PNMTX0 = view(N½) · view(N)⁻¹` for the refraction quad. That was written from the retired-era
handoff without checking the current mechanism, and **the current mechanism already does exactly
that, for every draw of that kind, unconditionally.**

`aurora::gfx::interp::begin_camera_delta` computes

    g_camDelta = V_lerp · V_cur⁻¹

once per tick (`extern/aurora/lib/gfx/interp.cpp`), and `interpolate_recorded_frame` applies it via
`patch_camera_only` to every draw that is **perspective and unpaired** — which is precisely the
water refraction: an immediate-mode `TDLTexQuad`, so it carries no `J3DShape::draw` tag, and its
`PNMTX0` is identity, so composing the delta onto it yields the eye-space reprojection verbatim.
The prescription and the implementation are the same matrix.

**Measured**, path A, camera rotating, per-region alternation (1.00 = both presents advance the
region equally; higher = that share moves on only one of the two):

| region | A — stream interpolation | C — record-and-replace (the CONTROL) |
|---|---|---|
| **sea / water** | **1.03** | **4.99** |
| sky | 1.02 | 5.27 |
| ground | 1.02 | 1.08 |
| buildings + palms | 1.11 | 1.15 |

The control is what makes this readable. Path C covers `J3DModel` draw matrices only, so
immediate-mode water and the background MUST snap under it — and they do, at ~5. Had the metric
returned ~1 for both paths it would have been blind, and "the water interpolates" would have been a
statement about the instrument.

**What this does NOT establish.** Alternation near 1.0 rules out the water SNAPPING — moving on one
present and not the other. It does not prove the reflection TRACKS the surface: a reflection that
swims coherently can still produce balanced step magnitudes. Per the standing directive that remains
a headed check, and it is the specific thing to look at: watch the tower reflection in the plaza
water while rotating the camera with the C-stick.

### What IS still missing

The honest gap is the one the coverage line names, not the water: **~9.5% of all draws are untagged
PERSPECTIVE INDEXED** — display-list geometry drawn from a persistent vertex array, which HAS a
stable cross-tick identity and should therefore be paired and lerped, but instead falls through to
`patch_camera_only` and receives the camera delta alone. That is correct for static scenery and
wrong for anything that moves in the world: such an object follows the camera but not its own
motion, so it snaps in object space inside an otherwise smooth frame. Each one is a tagging seam
that `j3d_capture.cpp` does not cover.

- `effects.h` / `effects_screen.cpp` already IDENTIFY the screen-sampling effects by name
  (shimmer, water refraction, bath mist, mirror pre-render) and record which fired each frame. That
  identification is the input the callback work needs; it is not yet wired to anything that acts.
- `effects_afterimage.cpp` already classifies the dash-trail EFB copy as cross-frame feedback so it
  advances once per tick rather than twice. It is the one effect that IS handled, and it is the
  worked example for the rest.
