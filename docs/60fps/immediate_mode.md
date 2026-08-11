# Immediate-mode geometry and 60fps — why the fountain jitters, and what fixing it requires

The fountain, the waving flags and the sea ripple grid all jitter under interpolation. They are one
problem, and it is not the one the matrix path can solve.

## The population, attributed

`SBR_TAGGAP=1` hooks `GXBegin` and attributes every immediate-mode draw to its caller:

| site | share | |
|---|---|---|
| `JUTResFont::drawChar_scale` | 26.9% | text glyphs — genuinely 2D, correctly snaps |
| **`JPADrawExecRotBillBoard::exec`** | **21.3%** | **JPA particles — the fountain** |
| **`TMapObjFlag::draw`** | **17.8%** | waving flags |
| `SMS_DrawCube` | 11.1% | the shadow's alpha-restore cube |
| **`TMapObjWave::draw`** | **8.6%** | the sea ripple grid |
| `JPADrawExecBillBoard::exec` | 1.4% | more particles |

## Why tagging them would do nothing

`JPADrawExecBillBoard::exec` (decomp `decomp/sms/src/JSystem/JParticle/JPADrawVisitor.cpp`):

```cpp
particle->getGlobalPosition(pt);
MTXMultVec(dc->pcb->mViewMtx, &pt, &pt);        // world -> EYE SPACE, on the CPU
GXBegin(GX_QUADS, GX_VTXFMT0, 4);
GXPosition3f32(offs[0].x + pt.x, offs[0].y + pt.y, pt.z);
...
```

The particle's position is transformed on the CPU and **baked into the vertex stream**. There is no
per-particle position matrix. So a cross-tick identity — and `JPABaseParticle*` is a perfectly good
one — buys nothing, because `patch_draw` interpolates the uniform block's position matrices and
there is nothing there to interpolate. The same is true of the flags and the wave grid, which build
their meshes on the CPU each tick.

This is worth stating plainly because "give it a tag" is the reflex after the shadow work, and here
it is a wasted change that would measure as a coverage improvement while fixing nothing.

## What these draws DO already get right

Because the vertices are in EYE space and the position matrix is identity, `patch_camera_only`'s
`V_lerp · V_cur⁻¹` reprojects them correctly for CAMERA motion — the same mechanism that already
handles the water refraction quad. So the fountain follows a moving camera smoothly. What steps at
30 Hz is each particle's own motion through the world, which is exactly what `pt` freezes per tick.

## What fixing it requires — and the cheap way, which is not vertex interpolation

The obvious answer is "pair these draws and lerp their vertex arrays". That is harder than it looks
and, for billboards, unnecessary.

### Why vertex lerping is expensive here

`gfx::push_verts(data + pos, totalVtxBytes, ...)` (`command_processor.cpp`) pushes the **raw GX FIFO
bytes**, not a decoded float array — aurora feeds the vertex stream to the shader and decodes there.
So lerping positions would mean decoding each draw's vertex format (the attribute set and component
type vary per draw), byte-swapping, lerping, and re-encoding. Worse, consecutive compatible draws are
MERGED into one `vertRange` (`canMerge`), so a merged range no longer corresponds to a single
taggable object. A vertex shadow would also have to mirror far more data than the uniform shadow,
which exists precisely because staging read-back dominated the frame.

That path is still the general answer for a deforming mesh — the flags and the wave grid genuinely
change shape per tick, and nothing but their vertices carries that.

### But a BILLBOARD does not deform — it translates

Re-read what the particle draw emits:

```cpp
particle->getGlobalPosition(pt);
MTXMultVec(dc->pcb->mViewMtx, &pt, &pt);
GXPosition3f32(offs[0].x + pt.x, offs[0].y + pt.y, pt.z);   // offs[] is the QUAD, pt is the POSITION
```

`offs[]` is the quad's shape from the particle's scale; `pt` is a single eye-space point added to
every corner. Between two ticks a particle's quad is the same shape displaced by `pt_cur − pt_prev`
(plus any scale change). And the position matrix for these draws is IDENTITY, with the vertices
already in eye space.

**So the whole correction is a translation, and there is already a per-draw matrix to put it in.**
Write `translate(−(1 − alpha) · (pt_cur − pt_prev))` into `PNMTX0` for that draw — composed with the
camera delta `V_lerp · V_cur⁻¹` that `patch_camera_only` applies to these draws anyway — and the
particle sits at its interpolated position with no vertex data touched at all.

What it needs:

1. **A tag**, from the two `exec` entries (US `0x8033025c`, `0x80330434`; `JPABaseParticle*` in r5).
   Mind the pooled-address hazard the shadow work hit — a particle at age 0 is a NEW particle
   whatever its address, so the generation can be bumped from that rather than guessed.
2. **The per-tag position**, recorded at the same hook: `getGlobalPosition` in WORLD space, so the
   prev/cur pair is not entangled with the view. Eye space would make `pt_cur − pt_prev` mix two
   different view transforms, which is the same trap the vertex path has.
3. **The write**, in `interpolate_recorded_frame`: for a tagged draw with a known delta, set the
   position matrix to the camera delta composed with that translation instead of the camera delta
   alone.

Scale changes are a second-order term and can be ignored initially or folded in as a matrix scale
about the quad centre; a particle whose scale changes materially between two ticks is rare.

This does not help the flags or the wave grid, which really do deform. It does cover both particle
paths — 22.7% of immediate-mode draws, and the fountain.


---

## LANDED (2026-08-06) — particles interpolate

`sms-recomp/frame_interp/tag_particle.cpp` + `aurora::gfx::interp::{set_tag_world_pos,patch_billboard}`.

Measured on a plaza run with the camera rotating:

* 517,119 billboard draws tagged and positioned; **200,122 had their own displacement applied**
  (95.2% of those reaching the patch), 10,194 correctly fell back to the camera delta alone because
  they were new particles or had skipped a tick.
* **The control fires:** with the path on vs off at the same guest tick, 0.588% and 0.507% of pixels
  differ on the in-between presents (max channel-sum delta 539 of 765) while the game's own main
  frames are **byte-identical**. So the write reaches the screen and does not leak into the frames
  the game renders.
* Mispairing unchanged: the 100–1k bucket reads 54, against 98 for the shadow default and 4 for a
  no-tagging control.

### Two instruments earned their keep

**`FLAG_JUST_BORN` is useless at the draw seam.** It was the obvious generation signal and measured
**0 bumps over 517,119 draws across 268 addresses** — because the flag is set at creation and
cleared during the particle's update, which runs before the draw pass. A zero meaning "always clear
by the time I look" is indistinguishable from "no particle was ever born"; it was caught only
because 268 addresses serving half a million draws obviously implies reuse. `mAge` replaces it: it
increases monotonically for a given particle, so for the same address a NON-INCREASE is a definitive
reuse test rather than a threshold. It reports 17,877 reuses.

**The tick stamp is one behind `g_tickIndex`.** `set_tag_world_pos` is called while the GUEST is
drawing; `g_tickIndex` is not incremented until `begin_camera_delta` at the frame seam after it. The
first version required `stampCur == g_tickIndex`, which is never true — it reported **0 patched and
375,451 unpaired**, caught in one run because that line prints both numbers instead of just the
successes.
