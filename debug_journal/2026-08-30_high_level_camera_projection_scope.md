# High-level camera projection scope

The native J3D model adapters no longer obtain projection from the GameCube graphics API. Recomp
previously read `sbr_gx_current_projection()`, a mirror written by `GXSetProjection`; native decomp
called `GXGetProjectionv()`. Those values happened to be ordinary matrices by the time they crossed
the semantic sink, but their owner was still the GX compatibility seam.

## Retail and decomp ownership

The game camera writes a complete 4×4 projection into `JDrama::TGraphics::mProjMtx` at offset
`+0x74` and the view matrix at `+0xb4`. `TPolarCamera`, `TLookAtCamera`, `TOrthoProj`, and
`CPolarSubCamera` all populate those fields before optionally calling `GXSetProjection`.

The common high-level boundary is `JDrama::TViewObj::testPerform`, not one scene class. It is the
dispatch funnel used by screens, camera connectors, perform lists, object managers, and J3D scene
groups. The native decomp source confirms that it applies the object's cue mask and then invokes the
virtual `perform` with the same `TGraphics*`. GMSE01 Ghidra confirms the retail body at `0x802fcc94`.
`TCamConnecter::perform` (`0x802fbc68`) dispatches the camera with projection cue `0x10` before
dispatching the child draw, and `TSmJ3DScn::perform` (`0x802fc644`) consumes `TGraphics + 0xb4`
before entering/drawing its J3D buffers. Scoping each surviving high-level draw dispatch therefore
captures the camera after it writes `TGraphics` and covers title paths that bypass `TSmJ3DScn`.

The European `TSmJ3DScn` symbol address `0x802f47c4` was initially tried against the US binary and
decompiled as an unrelated GX command writer. That result was rejected. The US functions above were
located and checked from their actual bodies rather than inferred through a regional offset.

## Runtime contract

`native-render/model_context` owns a fixed-capacity nested stack of copied projection values. It
recognizes the structural last row emitted by the game's camera constructors: `[0,0,-1,0]` for
perspective/frustum and `[0,0,0,1]` for orthographic, then performs the existing clip-depth
normalization once. An orthographic inner dispatch replaces, rather than inherits, an outer
perspective context and the outer value is restored on return.

The recomp guest adapter reads sixteen big-endian floats from `TGraphics + 0x74`. The native decomp
hook passes the native matrix directly. Both `J3DShape::draw` adapters accept only the current
perspective context and otherwise leave the shape to the always-retained original GX body. Neither
adapter includes or calls the GX projection cache, FIFO parser, BP/XF state, TEV, or EFB machinery.

The first guarded recomp attempt exposed a duplicate `0x802fcc94` override: the interpolation
subsystem already owned this funnel. The two behaviors were merged into that single override, which
now scopes projection and runs interpolation observation before one retained retail body.

The next run found 46 early draw-cue dispatches before a camera initialized `TGraphics`; one sample
had the finite garbage last row `[0,0,8.96831e-43,-4.23008e-38]`. This is a real setup lifecycle,
not a third authored projection. Such traversals push an explicit empty context so they cannot
inherit stale outer camera state. An unreadable `TGraphics` remains an assertion/panic.

A clean full rebuild also exposed stale positional `SemanticSink` aggregate initialization in two
tests. The previous model-sink field addition had shifted the intended context slot, but incremental
builds had not recompiled both targets. Both tests now use named fields and provide an
`unexpected_model` callback that asserts if their 2D-only boundary receives model traffic. Issue 35
records that separate test-contract defect.

## Controls and live evidence

The shared control feeds perspective, orthographic, unsupported, and non-finite matrices, proves
the depth conversion, and proves perspective → orthographic → perspective nested restoration plus
explicit empty-state masking. The recomp guest adapter independently proves big-endian perspective
and orthographic reads, rejects a planted structural mutation, and reports a truncated memory read.

- Guarded recomp, fastboot, 60 presents: 6,468 submitted models / 2,512,884 decoded vertices; zero
  unreadable, layout, rigid-matrix, or mesh-decode failures. High-level dispatches: 11,331
  perspective, 728 orthographic, 46 pre-camera. Forty-four model attempts outside perspective fell
  back. Exit 0 with no kernel GPU fault.
- Guarded native decomp, fastboot, 180 presents: 11,858 submitted models / 7,194,726 vertices; zero
  layout, rigid-matrix, or mesh-decode failures. High-level dispatches: 36,422 perspective, 1,713
  orthographic, 3,547 pre-camera. 12,154 model attempts outside perspective fell back. Exit 0 with
  no kernel GPU fault.

Final combined gates: root/native-decomp CTest 50/50, recomp CTest 34/34, seven shader sources
compile/validate/match, 22/22 diagnostic tool controls, Clang format on 17 changed files, Clang-Tidy
on 12 changed translation units, and the 457-file structure ratchet with zero violations. The
project-state and codemap validators report no broken links or ownership gaps.

The result is falsified if either semantic model adapter again reads GX/FIFO/compatibility state, a
recognized camera projection is not restored across nesting, a live perspective run submits no
models, or an original draw/dispatch body stops executing.
