# Semantic J2DGrafContext filled boxes

## Root cause

The shared PC-native 2D renderer already had a renderer-neutral solid-rectangle command, but only
`GC2D::fill_rect` published one. `J2DGrafContext::fillBox` still existed solely as retained GX
vertex emission in both runtimes, so any solid or four-corner-gradient box using that J2D owner was
absent from semantic frames.

## Retail contract

GMSE01 `J2DGrafContext::fillBox` is `0x802eba70`. The generated PPC body and the decomp source agree
on the inputs:

- `r3` is the graph context and `r4` is a four-s32 `JUTRect`;
- every coordinate is narrowed to signed 16-bit by the direct GX vertex write;
- the loaded position matrix is the context's 3x4 matrix at `+0x84`;
- colours are read at `+0x28`, `+0x2c`, `+0x30`, and `+0x34`;
- the third vertex is geometric bottom-right but receives `mColorBL`, while the fourth is geometric
  bottom-left but receives `mColorBR`.

The apparent bottom-corner name reversal is retail behavior. The semantic command's canonical
corner order is TL, TR, BL, BR, so its last two values are therefore read from `mColorBR` and
`mColorBL` respectively.

`J2DGrafContext::setScissor` normalizes the rectangle, shifts both vertical bounds upward one pixel,
and intersects it with the retail 1024x1000 guard. Those are physical target pixels, not logical
ortho coordinates; treating them as logical and scaling them through a non-default canvas would
move the clip a second time.

## Implementation

`native-render` now owns the one transformed signed-16-bit rectangle resolver, target-pixel clip
space, and a source label distinguishing generic J2D boxes from existing GC2D fades/bands. The
recomp adapter reads the exact big-endian fields and a retained-body override publishes the command
at `0x802eba70`. The decomp body calls a native-layout bridge at the same high-level point. Both
adapters use the current copied J2D canvas/scissor and then leave the original GX body intact.

The SDL semantic-frame statistics report `j2d-fill-boxes` separately. This prevents an existing
GC2D rectangle from being misread as proof that the new seam ran.

## Controls and runtime result

- The recomp adapter control uses wraparound coordinates, a non-identity transform, four distinct
  colours, a target-pixel clip, truncated input, and a degenerate rectangle.
- The production-linked decomp control drives real native J2D objects through the same resolver and
  verifies the same narrowing, transform, corner ownership, clip, and host-allocation boundary.
- All 43 root/decomp tests and all 30 recomp tests pass. Changed C++ passes clang-format and
  clang-tidy. The watched semantic GPU test completed with no kernel GPU fault.
- A guarded 180-present Delfino run reached gameplay and exited zero with no capture refusal or GPU
  fault. It did not organically call `J2DGrafContext::fillBox`, so this is runtime-safety evidence,
  not live seam coverage. The per-source counter is the falsifier for a future scene that does call
  it.

Two later attempts to obtain a deliberately narrow semantic-only summary failed closed before
launch because the external watcher could not establish a kernel-journal cursor. They are not game
failures and are not counted as runtime evidence. The watcher's planted positive/negative selftest
still passed; the original 180-present guarded run had already crossed its final kernel barrier.

## Remaining boundary

The next missing 2D family is `J2DWindow`: its interior and eight textured frame pieces require a
window-semantic command/material contract. Recomp address `0x802d18ec` is already owned by the
widescreen window override, so that work must refactor the existing owner rather than registering a
second override.
