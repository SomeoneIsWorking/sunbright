# Motion interpolation (N64Recomp-style) — design map

Goal: decouple SMS's 60 Hz game tick from display refresh and synthesize in-between
frames by interpolating each 3D model's transform between frame N-1 and frame N,
keyed by a stable per-model ID.

## 1. How SMS builds & manages 3D objects

SMS runs on Nintendo's **JSystem** libraries + the Sunshine actor framework:

- **JKernel / JKR** — heaps. The game arena is `0x80427820 – 0x817FEEC0` (seen at
  boot). Objects are `new`'d here once and **persist at a fixed address** for their
  lifetime → the object pointer is a usable cross-frame identity.
- **JDrama** — scene-graph / actor framework. Base class `JDrama::TViewObj` with a
  virtual `perform(u32 flags, JDrama::TGraphics*)`. Each frame the director
  (`TMarDirector`) walks the scene in passes: a **calc/move** pass then a **draw**
  pass (distinguished by bits in `flags`). Object hierarchy:
  `TViewObj → TActor → TLiveActor / THitActor → game actors`. Mario is `TMario`.
- **J3D** — model + animation. Each drawable owns a `J3DModel` instance that
  references shared `J3DModelData` (mesh, skeleton/joints, materials). The instance
  holds a `J3DMtxBuffer`: the array of **per-joint world matrices (3×4)** for the
  current frame, computed from the skeleton × the actor's world transform × animation.
- **JGeometry** — math types (`Mtx` 3×4, `Vec`, `Quat`).

Per frame, per object:
1. `perform(calc)` → actor logic/physics, advance animation, then `J3DModel::calc()`
   fills `J3DMtxBuffer` with per-joint world matrices.
2. `perform(draw)` → `J3DModel::entry()/viewCalc()/draw()` → each joint world matrix
   is multiplied by the camera/view matrix and loaded to GX as a **position matrix**
   (and a normal matrix); the shape's vertices are then drawn referencing that slot.

The authoritative per-object transform each frame is therefore the **J3DMtxBuffer**
(model space) and/or the **position matrices loaded to GX** (view space).

## 2. Where transforms reach the GPU (verified in our pipeline)

- GX FIFO base register holds `0xCC010000`; writes target the **write-gather pipe at
  0xCC008000** (base − 0x8000). 127 functions in the DOL write it (the GX library).
- **We already own this path** — `runtime/memory_bridge.cpp` routes gather-pipe
  writes to `GPFifo::Write*`. So every matrix that reaches the GPU passes through us.
- GX library state (`__GXData`) is around `0x803F43C0`.
- Matrices reach the GPU via `GXLoadPosMtxImm` / `GXLoadNrmMtxImm` (small leaf funcs
  in the `~0x80182200 / ~0x8022D1A4` region — exact address TBD), which emit an XF
  load command: `[0x10][ (n-1)<<16 | xfAddr ][ n × f32 ]`. A position matrix is 12
  floats (3×4) loaded to XF matrix memory (xfAddr < 0x100).

Note: decoding the raw gather-pipe byte stream is unsafe without a full GX-opcode
parser (vertex data can contain a `0x10` byte). Prefer hooking the loader functions.

### ⚠ Confirmed: SMS uses **indexed** matrices, not immediate loads
A runtime tap on the gather pipe (`SUNBRIGHT_GXCAP=1`, see `memory_bridge.cpp`) shows
only **~8 valid 12-word XF matrix loads per frame** (plus parser false-positives from
vertex data). So model matrices do **not** flow through the FIFO as
`GXLoadPosMtxImm` — J3D uses the **indexed-matrix** path: per-joint world matrices
live in a RAM array (the `J3DMtxBuffer`), the GP fetches them by index, and only a
small matrix **index** + the **array base/stride** (CP register loads) go through the
FIFO. The few immediate loads we see are projection/special matrices.

Follow-up: I also checked whether the position-matrix **array base** is set per draw
through the FIFO (CP `0x08` reg load) — it is **not** (no RAM-pointer array bases pass
through the gather pipe in-scene). So the base is configured once at init to a fixed
matrix buffer; per object the game writes joint matrices into RAM and the GP fetches
them by index. **The gather pipe therefore can't be the capture point for model
transforms** — confirmed empirically. (`SUNBRIGHT_GXCAP` remains as a useful GX-stream
diagnostic, and does capture the few immediate/projection matrices.)

**Implication — capture must be CPU-side at the `J3DMtxBuffer`:**
- **J3D draw hook (preferred):** find `J3DModel::draw` / `J3DModel::calcView` (or the
  per-actor draw) and `SUNBRIGHT_OVERRIDE` it. `this` = the model ID; `mMtxBuffer`
  holds all joint world matrices for the frame — read them directly from RAM.
- **Per-object struct read:** for Mario specifically, find the `TMario*` global and
  read his root transform from the struct each frame — the simplest first capture.

## 3. Stable model ID — yes, two capture levels

Because objects live at fixed heap addresses, **the object pointer is the ID.**

**(a) Semantic hook (recommended).** Wrap `J3DModel::entry` (or the actor's
`perform(draw)`) via our override/instrumentation system. `this` (the `J3DModel*` /
`TViewObj*`) is the model ID, and at that point the full `J3DMtxBuffer` (all joint
matrices, model space) is readable *before* GX submission. Clean ID, clean
transforms, and lets us special-case Mario via the `TMario*` global.

**(b) Universal hook (fallback).** Wrap `GXLoadPosMtxImm/NrmMtxImm` and record every
`(matrix, XF slot)`. Attribute matrices to an object by bracketing them with the
"current model" set in (a). Without (a), the ID degrades to a draw-order index —
fine while scene draw order is stable, which it largely is.

ID hygiene: match by pointer; if an ID wasn't present last frame (spawn) render it at
native (no interp); if the transform delta is huge (teleport / camera cut) skip interp
to avoid smearing across the cut.

## 4. Interpolation algorithm

Keep simulating at 60 Hz; synthesize inter-frames for a higher display rate.

Per game frame, store each model's joint matrices in a double buffer: `prev[id]`,
`cur[id]`. For an inter-frame at fraction `t ∈ (0,1)`, blend each joint matrix:
- **translation**: `lerp(p, c, t)`
- **rotation**: decompose 3×3 → quaternion, `slerp` (or `nlerp`) → recompose
- **scale**: `lerp`

Interpolate the **joint world matrices** (before skinning) so skinned meshes follow.
Render the inter-frame by replaying the draw with the interpolated matrices:
- cleanest with hook (a): overwrite `J3DMtxBuffer`, re-issue the draw; or
- with hook (b): replay the captured GX command stream substituting interpolated XF
  matrix loads.

## 5. Why Sunbright is well-suited

Two advantages over emulator-side hacks:
- We run the game's CPU as **native code we control** → hook any function cheaply
  (the override system) to capture transforms with real object IDs.
- The **GX stream is ours** (gather pipe) → we control what reaches Dolphin's GPU.

Proposed pipeline:
1. **Capture** — override `J3DModel::entry` (set current ID + snapshot J3DMtxBuffer)
   and/or `GXLoadPosMtxImm` (record matrices). Key by object pointer.
2. On each real VI present you hold `cur` (this frame) and `prev` (last).
3. **Synthesize** N inter-frames: build interpolated matrices, replay the frame's GX
   draws with them, present each. Pass through or separately handle 2D/HUD.
4. **Pacing** — drive extra presents between the game's VI swaps (Dolphin presents one
   XFB per VI; we insert interpolated XFBs).

Hardest parts: (i) re-issuing draws for inter-frames needs the captured primitive
stream *or* a second draw pass with overwritten `J3DMtxBuffer`; (ii) excluding things
that must not interpolate (HUD/2D, some particles, camera cuts); (iii) present pacing.

## Status of the RE (this pass)
Verified empirically with `SUNBRIGHT_GXCAP`:
- 3D model transforms do **not** flow through the gather pipe — confirmed three ways:
  only ~8 immediate XF loads/frame, and those are all J2D's **constant 2D matrix**
  (z=2 → XF slot 0); no RAM-pointer array bases appear in the FIFO; and `psq_st`
  matrix copies (which lower to `mem_w32`) aren't present either.
- The GX library is the leaf cluster `~0x8035D000–0x80363000`. The "12-FIFO-write"
  leaves there are BP/draw loaders, not the matrix loader — so finding `J3DModel::draw`
  by blind static heuristics is slow.

**Recommended unlock: a symbol map.** The public SMS decompilation emits a `GMSE01`
symbol map (name→address) covering JSystem/J3D/GX. Dropping it in lets us name
`J3DModel::draw`, `GXLoadPosMtxImm`, `J3DMtxBuffer`, `TMario`, etc. immediately —
turning the capture hook into a one-liner. Worth wiring symbol-map support into
`sunbright-recomp` (emit named functions) + the override registry (override by name).

## 6. Concrete next steps

1. **Pin addresses** (via `sunbright-recomp --disasm` + the J3D/GX call patterns):
   `GXLoadPosMtxImm`/`GXLoadNrmMtxImm` (confirm by the `0x10` XF-load opcode + a
   matrix-pointer arg), `J3DModel::entry/viewCalc/draw`, the `TViewObj::perform`
   vtable, `TMarDirector`, and the `TMario*` global.
2. **Capture hook** — `SUNBRIGHT_OVERRIDE` on `J3DModel::entry` to push the current
   `J3DModel*` and snapshot its joint matrices into a per-frame, per-ID store.
3. **Prototype** — capture Mario's root joint matrix for two frames, log prev/cur,
   verify the lerped midpoint is sane. Then scale to all joints/objects + the replay.

Tools already available for the RE: `sunbright-recomp --disasm <addr> [count]`, the
override system (`runtime/overrides/`), `SUNBRIGHT_DUMP` (frame PNGs), `SUNBRIGHT_DIFF`.
