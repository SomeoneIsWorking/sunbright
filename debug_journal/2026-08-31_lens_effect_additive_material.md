# Lens-glow effect material above GX

The title audit exposed 104 perspective-reached lens-glow observations that matched the same
single-stage colour program but were rejected by the semantic adapter. The rejection was not a GPU
failure: the guarded run initialized the AMD Radeon RX 6700 XT, completed 120 presents, and exited
0 with no kernel GPU fault or reset.

## Root cause

The J3D full pixel block for these glow surfaces uses source-alpha plus destination-one blending
(`blend=1/4/1`) and disables depth testing (`depth=0`, depth compare field `3`, depth write `0`).
The shared raster classifier only represented ordinary source-alpha and premultiplied-alpha
compositing, and its full-block matcher required depth testing. Consequently every exact glow
candidate fell back with `UnsupportedRasterPolicy` even though its colour program was understood.

## Change and evidence

`ModelBlendMode::Additive` now preserves source-alpha/destination-one semantics in the renderer-neutral
policy. The shared SDL 3D pass maps it to source-alpha for the source and one for the destination;
the classifier also carries the authored disabled depth test. The exact glow program is published as
`TexturedEffectMaterial`, whose modulation is the authored K0 RGB multiplied by the TEV register-0
alpha; no TEV or register identity crosses the semantic boundary.

Controls are production-linked: the CPU classifier test rejects a changed program and missing TEV
colour data; the GPU semantic-pass control distinguishes additive destination-one from ordinary
alpha by drawing a half-alpha red triangle over blue (the additive result retains the blue channel).
The post-change title audit reports `submitted=1092`, `models=1092`, `988 lit models`, and zero
effect-shape candidate rejections. Other lens variants remain explicit fallbacks because their
stage programs differ.

## Follow-up: register-colour glow variant

The next 52 perspective-reached glow draws were not the same program: `_mat_lens_fx_1` binds
`P_glow3` with stage bytes `c008f28fc138e670`. Decoding the GX fields from the tracked SDK packing
shows colour `C0 * TEXC` (register-0 RGB times texture RGB), and alpha `A0 * TEXA` with scale 0.5
(register-0 alpha times texture alpha, halved). The full pixel block is an exact strict
`GREATER 64` alpha test combined with `ALWAYS`, source-alpha/destination-one blending, and disabled
depth test/write. The shared classifier now publishes those values as `TexturedEffectMaterial`
plus a `GreaterThan64` raster threshold; the threshold sends byte 64 to discard and byte 65 to pass.

The classifier test is positive for the register-colour and half-alpha equation and for the exact
byte-64 policy, while the existing changed-program control remains negative. The guarded title audit
completed 120 diagnostic presents with `effect-material ... raster=0`, submitted 1,144 semantic
models (52 more than the prior 1,092), and exited 0 without a GPU fault or reset.

## Follow-up: texture passthrough hand variant

The next 52 perspective-reached hand surfaces use `c008ec8fc108e670`. Its colour fields are
`TEXC * ONE`, while the alpha fields match the already understood texture-alpha path. The earlier
effect classifier rejected it because the enabled diffuse channel was mistaken for a required
lighting contribution; the authored combiner cancels that contribution. It now publishes white
texture modulation through the same effect material. A changed-byte CPU control remains negative,
and the guarded title audit advanced all 52 instances to native submission with no GPU fault or
reset.

## Follow-up: two-stage constant-alpha glow variant

The next 22 perspective-reached glow draws were `_mat_lens_fx_9` with `P_glow5`. Their first
stage uses the same constant-times-texture colour program with `konstColorSelection=7`, which is
the GX 1/8 constant ramp rather than K0. The second stage (`c228f0f0c308f870`) passes the previous
colour and replaces alpha with the GX 2/8 constant ramp (`konstAlphaSelection=6`). The semantic
effect material now carries an explicit alpha replacement mode and the 3D pass uses a matching
fragment shader, so sampled texture alpha is not accidentally multiplied into this family.

The classifier test rejects a changed second stage. A guarded 60-present title audit advanced all
22 instances through classification, image decode, scene readiness, and native submission; the
watcher exited 0 and reported no kernel GPU fault or reset.

## Follow-up: interpolated register-colour glow variant

The next 22 perspective-reached glow draws were `_mat_lens_fx_8` with `P_glow2`. Their exact
one-stage program is `c008e28fc108e670`: the colour fields decode to
`K0 * (1 - TEXC) + C0 * TEXC`, and the alpha fields decode to `TEXA * A0`. This is a distinct
program, not another constant-ramp or passthrough case. The semantic effect material now maps it
to `modulation = C0 - K0` plus `additive = K0`, which is the shared textured equation
`TEXC * modulation + additive`; alpha remains multiplied by the register-0 alpha. No TEV or
register identity crosses the renderer boundary.

The CPU classifier control checks the decoded RGB difference, additive K0 term, and register alpha,
while changed program bytes remain rejected. A guarded 60-present title audit advanced all 22
instances through classification, image decode, scene readiness, and native submission, raising
coverage from 1,478 to 1,500 models. It exited 0; the GPU watcher reported no kernel fault or reset.
