# Primary-light textured material family

The title audit exposed 52 perspective-visible lens-glow models that were falling back before
texture decode. Their material state selected channel `0x0706` with one texture and the same
texture-times-raster stage program already owned by the shared lit-texture shader. The existing
classifier admitted only `0x070e` (general material diffuse) and `0x070f` (vertex diffuse), so this
was a channel-policy omission, not a new combiner equation.

`debug_journal/2026-07-03_palm_lighting_ambient_floor.md` establishes `0x0706` as one enabled,
clamped primary-light diffuse with spot attenuation. The classifier now records that choice and
limits the published lighting context to its first point light. It does not accept the other
lens-glow stage programs, whose equations still require separate reverse engineering.

Evidence from the guarded title run:

- Before the change: 728 submitted models, 624 lit models; the 52 glow variants were classified as
  unsupported material states.
- After the change: 988 submitted models, 884 lit models; all 52 glow variants reached image decode,
  perspective readiness, and native submission.
- The run exited with code 0 and produced no GPU fault or semantic-frame error.

The original recompiled draw body remains live after semantic submission, and the native-layout
adapter consumes the same shared classifier. Equivalent organically reached decomp evidence remains
blocked by issue 30.
