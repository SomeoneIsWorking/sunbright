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
