# High-level J3D raster policy in the PC-native renderer

## Root cause

The semantic model boundary already carried geometry, camera matrices, vertex colour, and decoded
textures, but it omitted the high-level J3D material's raster policy. `Semantic3dPass` consequently
used one generic policy for every accepted model: no culling, less-or-equal depth with writes, no
alpha rejection, and no blending. That was not a GX-emulation requirement; the missing values have
ordinary PC-renderer equivalents and are owned by `J3DColorBlock` and `J3DPEBlock`.

## Grounded ownership and representation

Both runtime adapters now publish copied renderer-neutral values for culling, depth comparison and
writes, alpha cutout, and blending. The native decomp adapter obtains them through the matching J3D
accessors. The recomp adapter reads the corresponding retail big-endian object fields.

Ghidra's decompile of retail-US `J3DMaterial::createPEBlock` at `0x802d77a8` establishes the compact
pixel-engine block vtables: opaque `0x803E0E64`, texture-edge `0x803E0E00`, translucent
`0x803E0D9C`, and full `0x803E0968`. The full block stores its alpha identifier and references at
offsets `+0x08..+0x0B`, blend fields at `+0x0C..+0x0F`, and depth identifier at `+0x10`. Cull mode
lives at `+0x16` in the retail LightOff colour block and `+0x40` in LightOn.

The alpha and depth identifiers are lookup-table indexes, but they are also deliberately packed
encodings: `calcAlphaCmpID` uses `(comp0 << 5) + (op << 3) + comp1`, and `calcZModeID` uses
`compare * 2 + enable * 0x10 + write`. `makeAlphaCmpTable` and `makeZModeTable` populate the reverse
mapping from those exact indexes. The recomp adapter's field extraction therefore matches the
native accessors without reading mutable GX state or adding guest table addresses.

The shared classifier accepts compact opaque, texture-edge, and translucent blocks directly. It
accepts a full block only when its alpha, blend, and depth fields exactly expand to one of those
same three policies and fog is absent. Dithering and GX's before/after-texture depth-placement bit
are deliberately not renderer-neutral material inputs: the PC shader's discard occurs before a
surviving fragment writes depth, and the native-renderer goal does not preserve GameCube output
quantization. Arbitrary full policies and fogged materials continue through the retained original
renderer; they are not approximated.

Fog absence is determined by `J3DFogInfo::mType == GX_FOG_NONE`, not by a null fog pointer. Live
full blocks commonly own a default fog object whose type is `NONE`; rejecting pointer presence
alone eliminated every otherwise-supported model in the guarded recomp falsifier.

## Falsifier caught during integration

The first guarded recomp rerun submitted zero semantic models. The adapter had classified only the
three compact block vtables, while the live scene used full blocks containing explicit equivalents
of the common policies. Treating all full blocks as supported would have hidden custom behavior, so
the correction parses the complete high-level fields and requires an exact supported match. The
next guarded run restored nonzero model publication.

A later review added fog coverage but initially rejected any fog pointer and also required
GameCube depth-placement and dithering fields to match. Its guarded control again submitted zero
models. That was a second useful falsifier: live full blocks commonly own a default fog object with
type `GX_FOG_NONE`, and depth placement/dithering are fixed-function execution details rather than
the ordinary PC material policy. The final rule reads the fog object's type, rejects only an active
fog mode, and omits those two GX-specific controls from the semantic boundary.

## Controls and live evidence

The CPU controls cover each compact family, exact full-block equivalents, one-field mutations,
unsupported blocks, invalid cull values, and guest/native extraction. The actual SDL GPU control is
run under the kernel watcher and requires visibly different answers for back/front/all culling,
alpha 127 versus 128 in both colour and textured shaders, replace versus source-alpha blending, and
depth-write enabled versus disabled. The shader build independently compiles, validates, and byte-
checks all eight tracked shaders.

- Recomp: guarded 60-present fastboot audit exited 0, submitted 6,006 cutout/back-cull models and
  1,092,366 vertices, completed all 30 semantic frames, rejected 682 unsupported textured raster
  policies rather than approximating them, and reported no GPU fault.
- Native decomp: the first cold run tripped the default 15-second in-process watchdog while Aurora
  was creating a pipeline. A guarded rerun with a 60-second diagnostic watchdog exited 0 after 120
  presents, submitted 2,278 cutout/back-cull models and 513,876 vertices, and reported zero layout,
  rigid-matrix, or decode failures.

These live runs prove the texture-edge/back-cull family is reached in both runtimes. The GPU control,
not the live scene sample, proves the opaque/translucent and other cull branches. The result is
falsified if either adapter reads GX/FIFO raster state, an exact supported material stops mapping to
the same policy in both layouts, a one-field custom policy is accepted, the GPU control stops
showing its known-different answers, or either retained draw body stops executing.
