# Semantic J2D picture boundary above GX (2026-08-28)

The project-owned SDL3 FIFO renderer is still a GX implementation even when its pixels match
Aurora. The first genuinely PC-native boundary is now at
`J2DPicture::drawSelf(int, int, Mtx*)`: both runtimes publish a renderer-neutral picture value before
their retained GX body runs.

## What the function actually provides

The recomp entry at `0x802cc7c0` receives the picture in `r3` and a parent 3x4 matrix in `r6`.
Geometry is not simply `mGlobalBounds`. `drawFullSet` first crops/anchors the local pane extent from
texture-0 size, binding, layout wrap, mirror, and flip; `drawTexCoord` then applies
`parentMatrix * mGlobalMtx`. The shared resolver implements that exact value-level contract. The
integer x/y arguments cancel before local geometry reaches the transform.

The layered material is also game-semantic rather than an arbitrary TEV capture: texture 0 seeds the
sample, later textures mix using the already-derived packed blend factors, black/white remaps the
result, corner colour multiplies it, and live inherited `mColorAlpha` supplies opacity. The picture
fields previously named `unk104`, `unk114`, and `unk130` are now named `mBlendColorWeights`,
`mBlendAlphaWeights`, and `mFlip` in decomp.

## Boundary limits

This entry does not carry active clip enable, logical canvas/projection, or a semantic ordering
context. `mScissorBounds` may be stale when clipping is disabled, so the command explicitly leaves
clipping disabled rather than manufacturing a plausible rectangle. Those values must come from an
enclosing J2D traversal/context seam.

Texture identity and sampler state are captured, but decoded immutable/versioned RGBA content is not
yet owned by the live semantic resource layer. The independent SDL3 `PicturePass` accepts such
decoded content and rendered a watched 16x16 control: known texture quadrants, clipping, half-alpha,
repeat determinism, and changed revision/content all produced the required answers without a kernel
GPU fault. The game adapters are not connected to that pass yet, so this milestone proves the shared
above-GX producer boundary and an offscreen semantic consumer, not a visible PC-native game frame.

## Asset decoding is not the renderer boundary

Game-authored textures still use GameCube tiled storage. Converting those files to ordinary RGBA8 is
an input codec, just as decoding PNG would be; it does not make the PC renderer a GX renderer.
`native-render/src/image_decode.cpp` therefore owns only span-based content decoding and revisioning.
It has no guest address, FIFO, GX command, TEV, Aurora, SDL, or GPU type. Runtime layout adapters own
where the bytes came from, and the semantic renderer owns only decoded image values.

The extraction also corrected two inaccuracies in the old compatibility decoder before reuse:
RGB565/RGB5A3 components expand by hardware bit replication, and CMPR uses the GX 3/8 blend while
retaining the midpoint RGB for its alpha-zero selector. Focused controls cover all eleven image
encodings, all three palette encodings, tiling and mip sizes, exact bounds, invalid indices, both
CMPR branches, and source/palette changes that must alter the content revision. This is enabling
resource work, not a claim that the game is visibly rendering through the PC-native path.
