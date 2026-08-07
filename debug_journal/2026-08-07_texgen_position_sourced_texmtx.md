# 2026-08-07 — 60fps: the texture matrices never interpolated, so projected images slid on moving surfaces

User report, from playing: *"I see 0 water improvements running ./play.sh --fastboot --60fps"*, then,
when the region measurement was put to them, the correction that made it tractable — *"the issue is
what happens during camera rotation/movement not still camera water"*.

Both halves were right, and the second one is what named the defect.

## The measurement that said the water was fine, and why it was blind

`docs/60fps/effects.md` carried a per-region alternation table showing the sea at **1.03** under
stream interpolation against **4.99** for the record-and-replace control — a proper control, a real
signal, and a correct conclusion about what it measured. Alternation asks whether consecutive
presents advance a region *by equal amounts*. It cannot see an image that slides across the surface
it is painted on, because that motion is smooth and evenly distributed between the two presents.

So the instrument was not wrong; it was answering a different question from the one the user was
asking. **Alternation near 1.0 rules out snapping, not misregistration.** That distinction is now
written into the doc beside the table.

## Root cause

A GX texgen sourced from `GX_TG_POS` reads the **raw vertex attribute** — the position *before* the
position matrix (`lib/gx/shader.cpp`, `vtx_attr(config, GX_VA_POS)`). Interpolation rewrites position
matrices: for a paired draw to the object's in-between pose, for an unpaired one to the in-between
viewpoint. It has never rewritten **texture** matrices.

Where the texture matrix is a projection through the camera, the two are then a half-tick apart: the
surface moves, the image painted on it does not. It is invisible with a parked camera and appears
the moment anything moves, which is precisely the report.

`patch_camera_only` carried a comment naming this for the unpaired case and stating the blocker —
*"That needs the texgen source at this seam, which it does not currently have."* That was the work.
What the comment did not anticipate is that **`patch_draw` has the identical hole**, and the paired
population is by far the larger one.

## The old claim this falsifies

`effects.md` asserted the water fix "IS ALREADY IMPLEMENTED — do not write it again", on the grounds
that the refraction is an immediate-mode quad whose `PNMTX0` is identity, so the camera delta already
reprojects it verbatim. Measured over ~15,000 ticks with the camera rotating:

    104,944 draws used a position-sourced matrix texgen
          0 of them had an identity PNMTX

The bare zero is only readable because the rejects were bucketed by distance from identity: 89,952
sat hundreds of units away (ordinary object-space geometry) and 14,992 — exactly one per tick — sat a
fixed 0.12 away. Printing that one showed a two-vertex, 1.12-scaled screen overlay with a ±0.5 UV
bias. Not water. **The construct the old text described does not occur in the recomp's Delfino
Plaza**, so the mechanism it credited with handling the water was never reached.

## The fix

For a **paired** draw no camera delta is needed, because the interpolated model-view already exists:

    texmtx = A · pnMtx        so   A = texmtx · pnMtx⁻¹
    in-between:  texmtx' = A · pnMtx_lerp

Exact — if the decomposition holds. It does not always: an object-locked projection has
`texmtx = A'·M` with no view in it and its UVs are **correct unchanged** under camera motion, so
rewriting it would be the corruption. One frame's state cannot tell the two apart.

**So it is measured rather than guessed.** `A` is recovered every tick and the correction applied
only where `A` came out the same as last tick. `A` is constant for a camera projection (it *is* the
projection); for an object-locked mapping `A = A'·M·pnMtx⁻¹ = A'·V⁻¹`, which moves with the camera.
The gate is therefore a discriminator run against both classes, and it separates them:

| | draws |
|---|---|
| used a position-sourced texgen | 50,344 |
| **STABLE** — re-composed with the interpolated pose | **34,217** |
| **UNSTABLE** — object-locked or animating, left alone | **1,733** |
| no previous tick to compare | 5 |
| singular model-view / >1 such matrix | 0 / 0 |

A gate that returned one class for everything would be describing itself. This one does not.

The **unpaired** case is also implemented: with an identity PNMTX the vertices are already in eye
space, so the texture matrix cannot be object-locked and the delta composes on the **right**
(`texmtx' = texmtx · camDelta`) — the opposite side from the position matrices, because it acts on
the vertex *before* the texture matrix does. Correct, and **inert in Delfino** per the measurement
above. Its report line says so in those words, so a zero there reads as "the construct was absent",
never as "the fix works".

## Verification

`SBR_INTERP_TEXMTX=0` is the A/B. Same scenario, matched guest ticks (t4802-4806), ON vs OFF:

    present 0: mean|d| 0.447  max 24   6% of pixels changed >2 levels
    present 1: mean|d| 0.554  max 63   7%
    ...
    changed-pixel bbox: x 0-1279, y 248-708

A band across the middle of the frame — projected surfaces — not a whole-screen change and not
garbage. Real frames are untouched by construction: the patch only ever writes the replay packet's
uniform block. The change does reach the following real frame through the dash trail, which is
cross-frame EFB feedback, which is why every present in the pair differs rather than alternating
ones.

**What is NOT established.** Whether the reflection now tracks the surface is a headed check and
belongs to the user — the standing directive in `docs/60fps/effects.md`. Headless capture cannot
force a misregistration artifact, and the metric that would be reached for (alternation) is the one
that was blind to it in the first place. The thing to watch: rotate the camera in the plaza and see
whether the reflection stays put on the water.

## Lesson

An instrument with a valid control can still be blind, and its control does not tell you so. The
alternation metric had a control that failed correctly (path C scored ~5 where path A scored 1.03),
which is exactly what made its 1.03 persuasive. What it lacked was a statement of what it does
**not** cover — and the one class of defect it cannot see is the one that was present.
