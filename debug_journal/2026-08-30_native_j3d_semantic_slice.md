# First PC-native J3D model slice

The semantic renderer now receives actual J3D model values above GX from both runtimes. This is not
FIFO replay under another name: the shared boundary contains triangle vertices, UVs, vertex colour,
model/view/projection matrices, one decoded RGBA texture, and sampler policy. It contains no BP/XF
registers, TEV state machine, EFB copy, FIFO ordinal, Aurora object, or game object. Both runtime
hooks always continue through their original draw bodies for A/B and fallback.

## Grounded material family

The initial proposed unlit/untextured family was absent in a live recomp census. The dominant
smallest exact family was instead unlit, one texture, texture coordinate zero/map zero, raster
colour channel 4, and stage bytes `c0 08 f8 af c1 08 f2 f0` (texture multiplied by authored
vertex/raster colour). The shared classifier accepts only that exact program and refuses one-byte
mutations. Retail-US CodeWarrior vtable stores used by the guest adapter were taken from the actual
constructors: colour LightOff `0x803E0D38`, LightOn `0x803E0CD4`, basic texgen `0x803E0C84`, TEV
blocks 1/2/4/16 at `0x803E0BE8`, `0x803E0B4C`, `0x803E0AB0`, and `0x803E0A14`, and rigid
`J3DShapeMtx` at `0x803E125C`. CodeWarrior stores the table address itself, not an Itanium-style
address point offset.

## Shared decoding and runtime differences

`native-render` now owns the one J3D vertex-layout/primitive decoder and the one ResTIMG base-image
decoder. Retail guest memory keeps display lists and indexed arrays big-endian. The native decomp
BMD loader keeps display commands/indices big-endian but swaps indexed numeric arrays and embedded
ResTIMG scalar headers to host order. The decoder therefore names array byte order separately;
planted controls exercise both representations. Encoded texel and palette bytes remain big-endian
in both paths.

The SDL model pass uses ordinary PC vertex/fragment shaders and a depth target. A known-positive GPU
control draws one triangle red without a texture, then binds a one-pixel green decoded texture to
the same geometry and requires a third hash distinct from both clear and red. Both 2D and 3D passes
use one immutable-revision upload/cache implementation. The cache retains recurring assets, evicts
least-recently-used images above 2,048 entries, and never evicts an image referenced by the current
submitted frame.

## Live evidence and falsifiers

- Recomp, final combined-tree guarded 60-present audit: 6,512 submitted model draws and 2,519,484
  decoded vertices, with zero layout, projection, rigid-matrix, or mesh-decode failures; the game
  and GPU watcher exited cleanly. A distinct earlier 60-frame cadence submitted 8,150 models and
  4,724,700 vertices, so the count is a run-bound coverage result rather than a universal constant.
- Native decomp, `./run.sh --diagnostic --runtime decomp --fastboot --quit-after 400 --run-secs 180
  -- SB_SEMANTIC_FRAME_MODE=audit`: 141,825 shape draws considered; 42,852 model draws and
  27,326,178 vertices submitted; zero layout, rigid-matrix, or mesh-decode failures; guarded exit 0.
  It explicitly rejected 55,825 unsupported materials and 43,148 non-perspective contexts.
- Native decomp, final post-integration 180-frame rerun after separating native pointers from guest
  numeric addresses: 38,343 shape draws considered; 11,858 model draws and 7,194,726 vertices
  submitted; zero layout, rigid-matrix, or mesh-decode failures; guarded exit 0.
- The first 60-frame decomp attempt reached only early boot rectangles and correctly produced zero
  models; it is not counted as positive evidence.

These measurements are falsified if either adapter begins consuming GX/FIFO state, if the retained
draw body stops executing, if the planted endian/program/texture controls stop producing their
opposite answer, or if a rerun no longer submits nonzero models with zero layout/decode failures.

## Deliberate remaining boundary

Subsequent 2026-08-30 slices moved camera ownership to the high-level `TGraphics` draw dispatch and
added high-level cull, depth, alpha-cutout, and blend policy for the exact common opaque,
texture-edge, and translucent J3D families. The semantic model boundary no longer reads either GX
projection cache. Details and current controls are in
`debug_journal/2026-08-30_high_level_j3d_raster_policy.md`.

This remains geometry plus one exact sampled material-program family, not a faithful complete J3D
renderer. Custom pixel policies, lighting, multiple texture stages, skinning, mipmapped samplers,
particles, and screen effects are not represented. Unsupported programs are refused and remain
visible only through the retained renderer; none are approximated into the first family.
