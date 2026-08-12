# The 2D capture's gates were assertions, and all four were wrong

2026-08-12. `SBR_FIFO_2D` captures orthographic geometry out of the GX FIFO for the native
renderer, because the J3D seam only sees `J3DShape::draw` and so never sees the HUD. It was
declining 74% of the orthographic draws it examined, and every decline sat behind a comment
explaining why that class could not be real 2D. None of those comments had been measured.

| the comment | what the stream does |
|---|---|
| lines are a debug overlay | 22,776 declined draws were `GX_LINES`, 4 verts each, textured |
| "J2D is direct-only" | 5,510 draws use indexed POS/TEX0 |
| "J2D never uses PosNrmMatIdx; capturing such a draw is a gate bug" | 5,510 draws carry it |
| "s16/f32 are what 2D actually emits" | 870 draws use u16 |

Final state: 34,601 of 34,615 orthographic draws decode. The 14 that do not are degenerate
line segments, which the hardware also draws nothing for.

## The part worth remembering is not the percentage

Three things here generalise.

**A decline reason you did not write is invisible, and the sum is what catches it.** Each
gate got its own counter and the report asserts they add up to the declined total. That
assertion is what surfaced 14 draws returning early from the triangulator with no counter —
a path that had simply never been named. Without the sum they would have sat inside whatever
reason happened to be largest and confirmed it.

**"Is this geometry we already have?" is answerable, and it is not the same question as "can
I decode it?"** Decoding a draw the J3D seam already captured would double-count it into the
scene, which is worse than declining it. The discriminator was distance to the last
`J3DShape::draw` capture in draw-command count: something the J3D seam owns sits within a
few commands of one. 0 of 5,510 did. That is what justified the work — not the count.

**A gate that accepts everything and a gate that was deleted print the same number.** So the
capture now scores its own output: draws with a per-vertex matrix index (the newly-enabled
class) against draws without one (the class that already worked). 5,510/5,510 fully inside
the clip volume, against a 76.3% baseline.

## …and that check, alone, could not have failed

This is the trap this project keeps walking into, so it is worth stating in full. A position
matrix resolved from the wrong bytes — or from an unwritten row, i.e. zeros — collapses every
vertex of a draw onto a single point. Under an **orthographic** projection that point is
`(P[3], P[7])`: a screen corner. Inside the volume. **A total decode failure would have
scored 100% resident**, which is exactly what the indexed class does score.

The residency check was therefore worthless on its own and had to be paired with a count of
draws with no extent, which is the shape the failure actually takes. 0 of 5,510, and 0 in the
baseline class.

The general form: when you validate a transform by asking "did the output land in a plausible
range", check where the DEGENERATE output lands first. If it lands in the plausible range,
the check cannot fail and is not a check.

## Fail-fast, kept

Every path that cannot resolve an input leaves the draw declined rather than decoding
whatever bytes are at the address: an unregistered CP array, an element that would run past
MEM1, a matrix row the stream never wrote. `g_posmtxSet` exists for the last of these
specifically — an unloaded matrix is *unknown*, not identity, and treating it as identity
would produce a plausible-looking drawable in the wrong place.

## What this still does not cover, and says so

* Whether the captured geometry is the RIGHT geometry. Nothing here compares against aurora.
* Per-vertex `TexMtxIdx`: the index bytes are consumed so the vertex stride is correct, but no
  texgen matrix is applied, so such a draw's UVs would be raw attribute values. Currently 0
  draws in the plaza. That line prints **unconditionally**, at zero, because a gap disclosure
  that only appears when nonzero makes "checked, none" and "never looked" the same output.

Registry: C043. C011 ("the HUD is absent because 2D/J2D geometry is NEVER CAPTURED") is
falsified on that half; whether the HUD now appears needs a native-renderer run, which is
gated behind `SBR_RENDER_APPROVED` and a human.

## Postscript: the measurements before this were run against a stale binary

Two of the runs in this arc reported identical numbers because the binary had not relinked,
and the reason it went unnoticed is that the build-error grep matched `" error "` with
spaces, while GCC emits `error:`. Every build "succeeded". The binary's mtime was older than
the source's the whole time.

Comparing the binary's timestamp against the source's before believing a run costs one
command and would have caught it immediately.
