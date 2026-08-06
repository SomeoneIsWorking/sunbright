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

`JPADrawExecBillBoard::exec` (decomp `JSystem/JParticle/JPADrawVisitor.cpp`):

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

## What fixing it requires

**Pair immediate-mode draws and interpolate their VERTEX ARRAYS**, which the path cannot do today —
`capture_replay_snapshot` shadows the uniform region only, so there is no previous-tick copy of the
vertex data to lerp from.

The pieces, in order:

1. **A tag.** `JPABaseParticle*` for particles (r5 at both `exec` entries, US `0x8033025c` and
   `0x80330434`); the object address for flags and the wave grid. Note the reuse hazard the shadow
   work ran into: a pooled address that is freed and reallocated pairs two unrelated objects. A
   particle at age 0 is a NEW particle whatever its address, so the generation can be bumped from
   that rather than guessed.
2. **A vertex shadow**, the same shape as the uniform shadow and for the same reason — the staging
   buffer is write-combined, so the previous values must come from ordinary RAM rather than a
   read-back. Cost is the concern: vertex data is far larger than the uniform block, and the uniform
   shadow exists precisely because reading staging back dominated the frame.
3. **The lerp, in the right space.** This is the part that is easy to get wrong. Tick N−1's vertices
   are in `view(N−1)` space and tick N's in `view(N)` space, so lerping them directly mixes two
   different frames of reference and the result is wrong whenever the camera moves. The previous
   vertices must first be reprojected into the current view — `V_cur · V_prev⁻¹` — and only then
   lerped; `patch_camera_only`'s existing delta then carries the result to the in-between view.
4. **A vertex-count gate**, exactly like the matrix path's: a particle whose quad count changed, or
   a mesh rebuilt at a different resolution, must snap rather than smear between two unrelated
   shapes.

Only step 3 is subtle; the rest is bookkeeping. Nothing here is speculative — every claim above is
read out of the decomp source or measured by `SBR_TAGGAP=1`.
