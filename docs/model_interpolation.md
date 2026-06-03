# Native rendering port + motion interpolation (N64Recomp-style) — design map

Goal: decouple SMS's 60 Hz game tick from display refresh and synthesize in-between
frames by interpolating each 3D model's transform between frame N-1 and frame N,
keyed by a stable per-model ID.

> ## 🛑 SETTLED DECISION (2026-06-03, user-ratified) — render scope
> **Own the object model; keep Dolphin's GPU.** Hook the game's scene-graph draws
> (MActor/J3D + J2D), build OUR per-frame per-object model (transforms keyed by object
> pointer), and drive Dolphin's proven GX→Vulkan backend from it. We own *what/where*
> (transforms, 2D layout, interpolated in-between frames); Dolphin keeps doing the
> rasterization. We do **NOT** rewrite the GX→GPU rasterizer from scratch — that was
> explicitly rejected (enormous, no interpolation benefit). Do not reopen this.
>
> This one interception layer serves BOTH open render problems:
> 1. **Widescreen — DONE natively (2026-06-03).** Removed the `.data` patch + Dolphin's
>    ForceWide. `runtime/overrides/scene_render.cpp` hooks **`GXSetProjection` (0x80362c34)** —
>    the universal projection point Dolphin's `AspectMode::Auto` heuristic reads — and squeezes
>    the projection by 0.75=(4:3)/(16:9): perspective m[0][0] → wider FOV; ortho m[0][0]+m[0][3]
>    → 2D shrinks toward centre so after the 16:9 present it's correct-aspect + CENTERED, not
>    stretched. 2D squeeze latches on the first 3D frame (boot logos stay 4:3). Title verified
>    1604×896 (16:9), logo centered. REMAINING: full-screen 2D *backdrops* are centered/squeezed
>    rather than *expanded* to fill 16:9 (the user's "expand backdrops") — needs per-screen
>    distinction of backdrop vs overlay (the J2DScreen hook below is the lever); and the squeeze
>    latch is global, not per-frame (a 2D-only screen after 3D would squeeze — rare).
>
> ### 2D-element identification (in progress) — the blanket squeeze isn't enough
> The user's next ask: squeezing every 2D element is wrong — specific elements need specific
> treatment. Pinned 2D elements (USA/GMSE01):
> - **Screen fader `TSMSFader`** — `draw` 0x8013fc88, `drawFadeinout` 0x8013fa54, `setDisplaySize`
>   0x8013f680. Fades/wipes must cover the WHOLE 16:9 screen; the squeeze leaves the side ~12.5%
>   uncovered. **RE results (verified):**
>   - `TSMSFader::draw` (0x8013fc88) is NOT the fade quad — it wraps the whole 2D draw pass (all
>     the screen's projections occur in its scope). **Do NOT flag it and exempt "during fade"** —
>     that un-squeezes EVERY 2D element and stretches the menu (tried, reverted in fac8faf).
>   - `drawFadeinout` (0x8013fa54) IS the fade fill: it does GX vtx setup + draws the quad using
>     the **current 2D ortho** — it sets no projection of its own. So the quad inherits the
>     squeezed ortho → covers only the centre 75%.
>   - **Fix to implement (verify on BOTH a fade frame AND a menu frame):** give the fade quad an
>     un-squeezed ortho. Either (a) in `ov_gx_projection` save the last ortho's original (un-scaled)
>     matrix bytes + guest addr; hook `drawFadeinout`, temporarily restore that matrix and re-call
>     `GXSetProjection` (recomp_raw 0x80362c34) so the quad loads full-range, super-call, done; or
>     (b) intercept the quad's GXPosition X coords and widen 0→-107 / 640→747. (a) is cleaner.
> - **2D-element identification tool — DONE (`SUNBRIGHT_2DID=1`, `runtime/overrides/scene_id.cpp`).**
>   Wraps J2DScreen::draw + J2DPicture/J2DTextBox::drawSelf and writes a compact per-screen inventory
>   to `scratch/2d_elements/elements.log`: each element's type, NAME, screen rect, object-ID.
>   **J2DPane layout decoded:** +0x08 type fourCC, **+0x10 NAME fourCC** (the .blo tag, e.g. `'yaji'`
>   arrow, `'shn0'` shine, `'n_0a'` digit, `'root'` screen), **+0x14/0x18/0x1c/0x20 bounds x0/y0/x1/y1**
>   (s32, 640×480 space). `tools/crop_2d_elements.py` crops each element from a SUNBRIGHT_DUMP frame
>   → a PNG. CAVEAT: rects are pane-LOCAL, so nested panes crop offset — absolute positioning needs
>   the parent-transform accumulation (the drawSelf matrix r6, TODO to decode; would also give
>   absolute rects in the log). Use this to classify elements for the per-element widescreen fixes.
> - **In-game HUD `TGCConsole2`** — coins/timer/balloons/telop (0x8014xxxx cluster). Drawn in the
>   2D ortho; the squeeze centres it in the 4:3 safe area. NEXT: decide per-element anchoring
>   (corner-anchored gauges should move to the 16:9 edges, not stay 4:3-centred).
> The right model is per-element: overlays/logo → centre (squeeze ok); fades/backdrops → fill;
> HUD → edge-anchor. Hook the J2D/element draws, classify by object, apply per class.
> 2. **Interpolation** — capture each object's transform, slerp prev→cur, re-present
>    inter-frames between VI swaps. Capture + lerp are already prototyped (see below).
>
> Entry points (USA / GMSE01): `MActor::viewCalc` 0x80239734, `MActor::entry`
> 0x802394c8, `TMario::calcView` 0x802446c0; GX matrix loaders pinned below.

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

## RE results (symbol-ported + working capture hook)

The public SMS decompilation map (`reference/sms_gmsj01_symbols.txt`, 38k symbols) is
for **GMSJ01 (Japan)** — addresses differ from our GMSE01 (USA) build, but the library
code is byte-identical, so functions port by **size fingerprint** (sequence of
consecutive function sizes). This is exact for small leaves; large funcs (whose recomp
boundaries differ from the compiler's) port via their callers/callees instead.

Ported so far (USA):
| function | JP | USA | how |
|---|---|---|---|
| `GXLoadPosMtxImm`  | 0x800AD72C | **0x80362E0C** | size seq `34 34 24 3C 30 40` |
| `GXLoadPosMtxIndx` | 0x800AD768 | **0x80362E48** | adjacent |
| `GXLoadNrmMtxImm`  | 0x800AD798 | **0x80362E78** | adjacent |

J3D capture targets (JP addrs, port next): `calc__8J3DModelFv` 0x800286F0,
`entry__8J3DModelFv` 0x800288F4, **`viewCalc__8J3DModelFv` 0x800289E4** (per-joint view
matrices — the prime hook), `gpMarioOriginal` .sbss 0x8040A378.

**Working capture hook:** `SUNBRIGHT_WATCH=<hexaddr>` (in `sunbright_bridge.cpp`
`Run()` + `dolphin_hook.cpp` `call_ppc`) observes any recompiled function without
replacing it and logs args + the 3x4 matrix at r3. Verified on `GXLoadPosMtxImm`:
captures live matrices, and the **r3 matrix pointer is a stable per-object key** (it
repeats per UI element). This is exactly the interpolation capture primitive — point
it at `viewCalc` (once its USA address is found via callers of `GXLoadPosMtxIndx`) to
grab per-J3DModel joint transforms keyed by `this`.

## ✅ Proven end-to-end (prototype)
Symbol-free, no J3D function needed:
1. **Input** — keyboard/auto → GCPad via the input override. The analog stick uses the
   `X`/`Y` axis override (−1..+1), *not* direction buttons (that was the bug that made
   the stick dead while buttons worked).
2. **Find a transform** — `SUNBRIGHT_AUTOCAP` drives Mario still/still/right/still/left
   and RAM-diffs; the two "still" snaps are a noise baseline. It surfaces real animated
   3×4 world matrices (e.g. `0x804045DC`, which also has a duplicate at `0x80427420`).
3. **Interpolate** — `SUNBRIGHT_WATCHMTX=<addr>` reads the 3×4 each frame and emits the
   midpoint. Verified: translation moves smoothly frame-to-frame and `MID` is exactly
   `lerp(prev,cur,0.5)`. That's the N64Recomp in-between frame on live game data.

Remaining to ship real interpolation: slerp the 3×3 rotation (lerp is fine only for
near-identity), pin the specific transform you want (Mario's model matrix vs camera),
generate the in-between *frames* (re-present with interpolated matrices between VI
swaps), and skip on spawns/cuts. The capture + math are demonstrated; this is plumbing.

## 7. Native-render port — pinned hooks & first increment (2026-06-03)

Scope confirmed above (own object model, keep Dolphin GPU). Pinned draw hooks (USA / GMSE01):

**2D path (J2D) — the widescreen-layout fix; VERIFIED firing on the title screen:**
- `J2DScreen::draw(int x, int y, const J2DGrafContext*)` — **0x802cfda8** — top-level 2D
  screen draw. Probed live (`SUNBRIGHT_WATCH=802cfda8`): fires thousands of times/frame at
  the title, `r3` = a stable `J2DScreen*` (object ID, e.g. 0x80ccb51c), `r4`=x (0), `r6`=the
  `J2DGrafContext` (defines the 2D ortho / viewport — the layout control point).
- `J2DScreen::drawSelf` 0x802d01c8, `J2DPicture::draw` 0x802ccef4, `J2DTextBox::draw`
  0x802d0b28 — leaf 2D elements (image / text).

  → **First increment — DONE (foundation):** `runtime/overrides/scene_render.cpp` hooks
  `J2DScreen::draw` and *super-calls* the original via the new `recomp_raw(addr)` (runtime
  `SUNBRIGHT_RENDERPORT=1`). Verified: the hook fires, the original draw runs, the frame is
  unchanged (no regression) — i.e. we can now wrap any draw, observe/adjust state, then run the
  real thing. This is the mechanism the whole "own the draw path" port stands on.
  Finding: the J2DScreen's `J2DGrafContext` is a **fixed object at 0x80426fb4**; its ortho bounds
  are NOT plain floats in the first 0x40 bytes (mostly 0/nan there — likely behind a vtable / in a
  JUTRect member further in). NEXT: RE the GrafContext ortho (or hook `J2DGrafContext::setPort`/
  `place`), then adjust it to center overlays + expand backdrops for 16:9; verify the logo centers
  by frame dump. (`.data` constant patching in `widescreen_patch_tick` can't reach per-screen 2D
  layout — see `sms_widescreen.cpp`.)

**3D path (J3D/MActor) — interpolation capture:** `MActor::viewCalc` 0x80239734,
`MActor::entry` 0x802394c8, `TMario::calcView` 0x802446c0. NOT yet probed live — the autotest
didn't advance past the (2D) title into a 3D scene in the headless runs; verify these fire once
a 3D scene (file-select Mario / gameplay) is reached, then snapshot the J3DMtxBuffer keyed by
`this`. Capture+lerp math already prototyped (§"Proven end-to-end").

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
