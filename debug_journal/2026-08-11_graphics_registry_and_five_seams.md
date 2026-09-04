# The graphics registry, and the five populations it found snapping (2026-08-11)

## What was built

`docs/graphics/graphics_db.tsv` — a census of every source of visual output the port has been
observed to draw, written by the game itself. Detection is automatic at the three GX waists
(`GXCallDisplayList`, `GXBegin`, `J3DShapeDraw::draw`); each emitter gets a population id, the label
rides the stream as `GX_AURORA_DRAW_POP`, and aurora files every draw under the emitter that
produced it. Measured columns are rewritten every run; `re` and `note` are curated and never touched
by the game. Design and caveats: `docs/graphics/README.md`.

## Findings that outlive this session

**Symbol resolution must come from authoritative discovered-function metadata, not `sms_gmse01_funcs.txt`.**
That list omits weak methods, so an address inside one resolves to whatever function precedes the
gap plus an offset — which reads as an answer. It reported three distinct emitters as
`TMapWire::drawUpper+0x48 / +0x17c / +0x258`; `drawUpper` contains exactly one `GXBegin`, and two of
them were in `drawLower`. `g_recomp_table` has the real boundaries.

**SDK draw helpers must attribute to their caller.** `GXDrawCube` builds its own geometry, so every
cube in the game collapsed into one row named after the helper. Redirecting one frame up split it
into two `TMario::perform` sites — which then turned out to be Mario's occlusion-probe boxes, and
interpolable.

**`camera-only` was hiding two different faults.** Some of those draws move with an object (fixable
by an identity) and some must not move at all (screen-space under a perspective projection, which
the orthographic test cannot see). The second class had no mechanism; it now has
`GX_AURORA_DRAW_EXACT` and a `snap:EXACT` outcome. The flag is ONE-SHOT, consumed by the draw it
precedes, because `drawShineShadowVolume` emits both an exact screen quad and interpolating
sphere-slice display lists from the same call and a latch would freeze the slices.

## The five seams, and the identity each needed

| population | identity | why that one |
|---|---|---|
| JPA particles (all 9 per-particle visitors) | particle address + generation | every visitor adds `getGlobalPosition()` to a shape; eye-space and world-space variants both correct under the same patch |
| TMapWire | object + strip index | three strips of IDENTICAL vertex count, so an object-only key would pair a strip against another strip of the same rope |
| water mirror | object + fan index | two unconditional 10-vertex fans per call, same problem |
| Mario's cubes | call site | one TMario, one draw per site per tick — and the assumption withdraws its own tag if a site ever draws twice in a tick |
| shadow alpha cube | the group's MEMBERSHIP (hashed owners) | groups are re-clustered every tick and every box has 24 vertices, so the count gate cannot catch a group that changed composition |
| particle stripes | emitter address | no monotonic emitter age exists to make a generation from; the consecutive-tick requirement bounds the alias window to a same-tick recycle |

Result: after these, the plaza has **no camera-only population left**. World geometry, shadows,
particles, wires, mirror, stripes all interpolate; the 2D populations snap correctly; two screen-mask
populations are EXACT by declaration.

## Dead ends and things deliberately not done

* `TMario::perform`'s THIRD `GXDrawCube` (the silhouette box, MarioMain.cpp:287) is not in the
  singleton list: it never drew in a recorded run, so its address would have been derived from an
  offset rather than observed. When it draws, the registry gets a row and the row adds the entry.
* `JPADrawExecLine` and `JPADrawExecRotYBillBoard` are absent from the US function list, so they are
  named as missing rather than guessed at.
* Deleting `graphics_db.tsv` to "regenerate" it silently discards every curated verdict. It happened
  once in this session; `git checkout` recovered it.
