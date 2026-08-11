# Native rendering port + motion interpolation (N64Recomp-style) — design map

> **Paths in this document (reconciled 2026-08-12).** It predates the June-era layout
> reorganisation and the 60fps rewrite, so most `runtime/overrides/*.cpp` paths below name files
> that no longer exist. Where the mechanism survives, it moved:
>
> | named as | now | confirmed by |
> |---|---|---|
> | `runtime/overrides/scene_render.cpp` | `sms-recomp/runtime/render/scene.{h,cpp}` | holds the `GXSetProjection` (0x80362c34) hook |
> | `runtime/memory_bridge.cpp` | `sms-recomp/runtime/devices/dev_gxfifo.cpp` | the gather-pipe route |
> | `runtime/overrides/hud.cpp` | `sms-recomp/overrides/hud.cpp` | same file, qualified |
>
> `runtime/overrides/scene_id.cpp` and its `SUNBRIGHT_2DID` switch are GONE — no such file and no
> such switch is read anywhere in the tree. Treat that paragraph as a record of what was once
> built, not as a tool you can run.

> ## 🛑 ARCHITECTURE RULING (user, 2026-06-12; AMENDED 2026-07-11) — object level, NOT stream level
> A FIFO-replay design (capture gather-pipe bytes, strip tokens, patch CP
> ARRAY_BASE loads, re-push through GPFifo, juggle XFB presents) was built and
> verified (commits "60fps interp M-A/M-P": gx_stream.cpp assembler — 0 FIFO
> errors over 3968 held frames; gx_parse.cpp analyzer — 896/896 frames parse,
> ~1100 mtx-array patch points/frame) and then REJECTED by the user as
> emulation-layer engineering: it deepens the Dolphin dependency the project is
> deleting. Those modules stay (env-gated, off) as native-renderer-arc
> groundwork ONLY. Interpolation is built at the JDrama/J3D OBJECT level.
>
> **Amendment (user, 2026-07-11):** the FIFO-replay ban is **scoped to
> motion interpolation** (a production feature that re-issues frames at 60 Hz).
> It does NOT prohibit a **diagnostic FIFO parity harness** — an offline
> `SB_FIFO_REPLAY=<path.dff>` mode that replays a captured Dolphin GX FIFO
> through aurora's renderer, dumps the framebuffer, and compares it
> pixel-for-pixel against Dolphin's render of the same FIFO. The harness is a
> verification tool (same GX input, two renderers, controlled experiment), not a
> runtime dependency on Dolphin: it consumes a static `.dff` file, no Dolphin
> code. This is explicitly permitted. See
> `debug_journal/2026-07-11_fifo_replay_no_calldl.md` + the `SB_FIFO_REPLAY`
> implementation in `sms-boot/runtime/fifo_player.cpp`.

> ## The PC-port design: decouple the render pass from the game tick
> Game simulates at 30 Hz; render every 60 Hz field. On the in-between field,
> re-issue the engine's own draw pass with per-model draw matrices blended
> between tick N-1 and tick N (both already live in guest RAM — J3D
> double-buffers mDrawMtxBuf, swapDrawMtx at viewCalc top).
>
> Pinned pieces (USA / GMSE01, RE'd 2026-06-12):
> - **Draw pass** = TMarDirector::direct's (0x80299838, VERIFIED) draw branch:
>   perform(0xffffffff, &graphics) over member lists +0x40, +0x38, +0x3C,
>   +0x1C mPerformListGX, [+0x20 silhouette if gpSilhouetteManager->unk48>0 ||
>   gpCamera->unk2C8 != -1], +0x24 mPerformListGXPost, then GXInvalidateTexAll.
>   TGraphics = plain 0x100-byte struct, zero-initialized, unk2=0 for the draw
>   branch; cameras/viewports fill it during the pass.
> - **Cadence hook**: VIWaitForRetrace (native override, sms_vi_native.cpp)
>   runs 2.05×/game frame (measured) — the second call per game frame IS the
>   in-between field, on the game thread.
> - **Blend, race-free, no game-state corruption** (redraw-mode J3DModel::
>   viewCalc override 0x802deeb8 — does NOT call the guest body):
>   write lerp(buf0[i], buf1[i], ½) into buf0 (prev draw-mtx buffer: dead until
>   the next real swap), swap mDrawMtxBuf[0]/[1][viewNo] pointers for the
>   redraw, swap back after the redraw frame. Frame N stays intact in buf1 as
>   the next blend's source; buf0's blend data lives until frame N+1's viewCalc
>   overwrites it — the same double-buffer lifetime the game's own GPU reads
>   rely on. Nrm 3x3 buffers (+0x68) ditto. Teleport/cut guard: per-model
>   translation delta over threshold → no blend (copy buf1).
> - **Copy + present**: after the redraw pass, issue the display copy
>   (IssueGXCopyDisp 0x802f917c / the TApplication endRender path — exact args
>   RE pending) into the alternate XFB; present scheduling at the VI
>   apply_flush seam (sms_vi_native.cpp owns the shadow-reg→VI-MMIO apply).
>
> Staging: (1) redraw WITHOUT blend on the in-between field — re-render frame N
> verbatim; verifies the draw pass re-issues cleanly (no double-tick of
> particles/anim/state, drawsync stable). (2) blend redraw → real 60 fps.
> (3) artifacts pass: slerp upgrade if component-lerp rotation shows, exclusion
> list (2D/HUD lists already separate), camera-cut detection.
>
> ### Stage-1 findings (2026-06-13, runtime/overrides/interp_redraw.cpp)
> - A fabricated zeroed TGraphics corrupts the pass — direct() reuses ONE
>   TGraphics across calc+draw; cameras/viewports populate it in the CALC
>   passes. Fix: snapshot the live TGraphics when the game performs the GX list
>   (TPerformList::perform tee, r5 = TGraphics*), redraw with the snapshot.
> - Redraw-then-Unknown-Opcode was NOT stream corruption from the redraw: with
>   the gx_parse analyzer armed, every captured frame (game + redraw) parsed
>   clean while the CP still read garbage. FALSIFIED along the way: guest stack
>   overflow into the FIFO ring (headroom measured 63 KB at redraw entry; stack
>   0x804177e4..0x804274a0 is not adjacent to the ring 0x80448d60+).
> - ROOT CAUSE: CP FIFO ring OVERFLOW — CPReadWriteDistance 0x82CA0 > ring size
>   0x80000 (writer lapped the reader; hundreds of "FIFO is overflowed by
>   GatherPipe" warnings per run). Pre-existing roadmap item #6 (unthrottled
>   production since the backpressure wait was removed), amplified by the
>   redraw's 2x command volume. Resolution for the OPTIONAL redraw: admission
>   control — sunbright_cp_fill() (dolphin_hook.cpp) reads ring occupancy; the
>   in-between frame is only inserted under 50% fill, else that frame stays
>   30 fps. The mandatory-stream overflow remains item #6 (dies with the native
>   renderer).

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
>   (s32, 640×480 space). `tools/render/crop_2d_elements.py` crops each element from a SUNBRIGHT_DUMP frame
>   → a PNG. The log uses the pane's GLOBAL (absolute screen) rect at +0x24/0x28/0x2c/0x30, so
>   nested panes crop correctly too (verified: 'shn0' → shine icon, 'yaji' → OPTIONS arrow). Use
>   this to classify elements for the per-element widescreen fixes (full-screen fill vs edge-anchor).
> - **✅ HUD FULLY OWNED (2026-06-04) — `sms-recomp/overrides/hud.cpp`.** The in-game HUD's per-element
>   widescreen layout is now owned natively. Every HUD picture draws through
>   `J2DPicture::drawFullSet(this,x,y,w,h,…)` (0x802cc838), dest rect in the ARGS, so `hud.cpp` takes
>   over that function and rewrites x per element. Each element is classified by its `.blo` NAME
>   (J2DPane fourCC at this+0x10) into LEFT / CENTER / RIGHT and shifted toward its 16:9 edge by the
>   pillar width `off = 320·(1−scale)/scale` (≈107 px at WS_SCALE 0.75). The ortho squeeze already
>   gives correct aspect (round gauge), so ONLY x is translated — width/height/Y untouched.
>   **Verified element map (Delfino save):** LEFT = `s_*/d_*/c_*` (top-left counters: shine/coin/fruit);
>   CENTER = `m_*` (top-centre lives) + `go00/01/02` (health sun); RIGHT = `w_t0` (FLUDD water gauge).
>   **IDENTITY, NOT A FLAG:** no `g_in_hud`-style flag — that leaks across the tail-recursive scene
>   draw (it stretched/shifted the menus twice). The file-select menu ALSO uses drawFullSet (names
>   `s_0a/.s_1/n_0a/shn0/yaji/.x_0`); we match ONLY exact HUD role suffixes (`_ba/_ic/_tx/_x/_n<d>/_t<d>`,
>   or `go<NN>`) so the menu's `_0<x>` roles never match — proven live: every menu element classifies
>   `anchor=-` (x unmodified, original draw run verbatim → menus byte-identical). Verified frame:
>   `scratch/hud_native_owned.png` (counters hug top-left, water gauge hugs bottom-right, all correct
>   aspect). The long investigation below is kept as the trail; the answer was drawFullSet's arg rect +
>   per-element name classification. (Separate open item: the bottom subtitle/fade black bar — that's
>   the fade-curtain, not the HUD.)
> - **In-game HUD `TGCConsole2` — NOT J2D (the menus are; the HUD isn't).** Verified: with a save
>   state the game reaches Delfino (dolpic5), but no J2DScreen/J2DPicture/J2DTextBox draw hook fires
>   for the HUD — it draws via its `perform`, an *unnamed virtual* (hence hidden in the symbol list).
>   **Found by vtable RE: `perform` is vtable slot 8 for every `JDrama::TViewObj` subclass** (use a
>   class whose perform IS named, e.g. `TPauseMenu2::perform` 0x80155788 = its vtable[8], to fix the
>   slot; then read the target class's vtable[8]). TGCConsole2's vtable is at 0x803c0304, so
>   **`TGCConsole2::perform` = 0x8014083c** (confirmed: `rlwinm.` on r4=flags for the draw pass, then
>   reads `this` enable bytes + draws). Vtable address comes from the class's ctor/dtor lis/addi; read
>   `.data` from `scratch/bin/sms.dol` (the `--disasm` tool only reads code). NEXT: hook
>   TGCConsole2::perform, find the HUD's 2D ortho, and EDGE-ANCHOR the corner gauges to the 16:9 edges
>   (a uniform squeeze would wrongly centre them). The FLUDD water gauge is likely a separate TViewObj
>   — find its perform via vtable[8] the same way.
>   **✅ SOLVED (2026-06-04) — no-stretch edge-anchored HUD.** The in-game HUD draws each picture
>   element through **`J2DPicture::drawFullSet` (0x802cc838)**, which takes the dest rect (x,y,w,h) as
>   ARGUMENTS — so we own each element's position directly (found by `SUNBRIGHT_HUDCALLS`, which logs
>   every fn called during the HUD draw). Fix in `scene_render.cpp`: squeeze the HUD's 2D ortho ×0.75
>   like the menus (correct aspect), and in a `drawFullSet` override spread each element's x by 1/0.75
>   about centre 320 → squeeze×spread = the game's authored position (anchored) at correct size
>   (un-stretched). Menus use `drawSelf` not `drawFullSet`, so they're untouched. Verified: HUD at the
>   16:9 corners, round shine/water-gauge (not oval); title + file-select still correct. The long
>   investigation below (vtable RE of TGCConsole2::perform, scissor/viewport ruled out, the indirect-
>   draw dead-ends) is kept as the trail; the answer was drawFullSet's arg-level rect.
>   --- (historical) ---
>   **DONE (edge-anchor) + the no-stretch problem:** `scene_render.cpp` hooks TGCConsole2::perform,
>   flags the HUD window, and gives the HUD ortho its own squeeze factor `SUNBRIGHT_HUD_SCALE`
>   (default 1.0 = no squeeze → gauges fill to the 16:9 corners; 0.75 = full squeeze → centred,
>   correct-aspect; tune between). Verified: at 1.0 the coins sit top-left, WATER gauge bottom-right.
>   FUNDAMENTAL no-stretch issue: the 4:3 EFB presents at 16:9, stretching ALL EFB content ~1.33×, so
>   un-stretching requires PRE-SQUEEZING (×0.75), which CENTRES — you can't both keep correct aspect
>   AND reach the edges with one linear ortho (size+position are coupled). True no-stretch edge-anchor
>   = per-element: pre-squeeze each HUD element AND reposition its scissor/ortho to its corner. The HUD
>   (0x803630c8) is full-screen. **Investigated 2026-06-04 — NO clean per-element handle:** logged
>   GXSetScissor (0x80363138) AND GXSetViewport during the HUD. Scissor is set ONCE per HUD frame to a
>   single bottom strip (left=45 top=389 w=455 h=26 = the subtitle line), NOT per gauge; viewport is
>   full-screen. So the corner gauges are positioned by their GEOMETRY inside a full-screen 2D ortho
>   (m00=2/640, m03=-1) — no per-element scissor/viewport/ortho to retarget, and one linear ortho
>   can't anchor BOTH corners (a single m03 shift moves all elements the same way). A true no-stretch
>   anchor needs: (a) RE TGCConsole2::perform (0x8014083c) element-position fields — it reads each
>   element's x/y from this+offsets; shift left-corner elements' x negative and right-corner positive
>   so that after the ×0.75 pre-squeeze they land at the 16:9 edges (correct aspect, anchored); or
>   (b) vertex-level interception (very deep). The SUNBRIGHT_HUD_SCALE knob is the practical interim.
>   Gameplay capture for iteration: SUNBRIGHT_SAVE_ON_HUD auto-saves at the HUD
>   (scratch/hud_gameplay.sav); load it with SUNBRIGHT_STATE for instant HUD iteration.
>   **No-stretch MATH (worked out):** the elements are ALREADY at the right anchors under the
>   exemption (HUD_SCALE=1.0) — only the size is stretched. Clean fix = FULL-squeeze the HUD ortho
>   (m00 AND m03 ×0.75 → correct aspect) AND spread each element's stored position ×1.333 about
>   centre 320 (x' = 320 + (x−320)×1.333); spread×1.333 then squeeze×0.75 = no net position change
>   (stays anchored) but size ×0.75 (un-stretched). **Blocker:** must do it per-element, and the
>   element draws are not yet pinned. perform (0x8014083c) calls 0x8013ebf0 ×6 (a counter draw that
>   reads pos at element+0x18) but those 6 are CONDITIONAL and NOT called in normal Delfino gameplay
>   (verified — hook never fired). The visible coins/shine/water/lives/dark-overlay are drawn by the
>   OTHER calls from perform: 0x8014ce84, 0x8014cc20, 0x8014c7e8, 0x80148f64 (+ indirect). NEXT: hook
>   each of those against scratch/hud_gameplay.sav, see which draws each visible element + its
>   position field, then spread+squeeze per element. (The width-constant approach was tried and
>   reverted — it bunched the whole HUD left; the elements aren't positioned via those constants.)
>   **Further finding (2026-06-04):** NONE of perform's direct bl targets fire in Delfino gameplay
>   (verified: 0x8013ebf0, 0x8014ce84, 0x8014cc20, 0x8014c7e8, 0x80148f64 hooks never hit). So the
>   visible coins/shine/water/lives are NOT drawn by perform's direct calls — they're drawn
>   INDIRECTLY (each is almost certainly its own JDrama::TViewObj child object, drawn via its own
>   perform = vtable[8] dispatched indirectly; TGCConsole2::perform handles the dark overlay +
>   dispatch). So the per-element port needs, PER ELEMENT: identify the child object (e.g. from a
>   TGCConsole2 field / the scene graph), find its class + vtable + perform, hook it, spread+squeeze
>   its position. That's a multi-object RE effort. The working/shipped result remains the
>   SUNBRIGHT_HUD_SCALE knob (1.0 edge-anchored+stretch ↔ 0.75 centered+correct-aspect).
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
