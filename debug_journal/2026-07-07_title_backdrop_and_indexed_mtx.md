# 2026-07-07 — Title backdrop bring-up: indexed matrix loads landed; "red screen" falsified

## Status correction first (falsifies this morning's note)

The title screen is NOT missing its backdrop color: the "red background" in earlier
screenshots was MY dump conversion reading the BGRA8 swapchain dump as RGBA (red↔blue
swap). Converted correctly (`magick ... bgra:`), the current title renders: golden Shine
sprite, red sun logo, palm tree, rainbow logo shadow, and TSky's dark-blue GXDrawSphere
backdrop (Sky.cpp:73 mat color (0,0x12,0xEE) — sampled pixel (0,0x10,0xC3), the small
delta is TEV/blend scale, not a defect I chased). SB_DUMP_FRAME's comment now says BGRA
loudly. Reference image: `scratch/screenshots/title_mtx_true.png`.

## What actually landed (all aurora)

1. **GXLoadPosMtxIndx / GXLoadNrmMtxIndx3x3 implemented** (were silent no-op TODOs):
   they emit GC-encoded CP LOAD_INDX commands (u32 = index<<16 | (len-1)<<12 | xfAddr),
   giving the same deferred-fetch semantics as GC hardware (the fifo processor reads the
   pool at drain time). J3D's DEFAULT matrix pipeline (PNGP, J3DShape.cpp:18-22) loads
   BOTH pos and normal matrices through these — every J3D shape drawn via
   J3DShapeMtx::load depended on them. Removed the shadowing no-op stubs from
   sms-boot/runtime/sdk_stubs.cpp (an executable-level stub silently WINS over the
   aurora lib symbol at link — trap worth remembering).
2. **CP_CMD_LOAD_INDX parser fixed**: previous decode read 1 byte of index + misaligned
   addr/len and computed `arrayType = GX_POS_MTX_ARRAY + (opcode - (CP_CMD_LOAD_INDX_A /
   0x08))` (indexes far out of range — this path had never been exercised). Now GC
   encoding, ASSERT on missing array base, and reads with the ARRAY's endianness.
3. **3-arg GXSetArray shim endianness**: le=false (BE) is right for file-origin vertex
   arrays but WRONG for the runtime-computed pools (J3D draw/normal matrices, lights)
   that GX_POS_MTX_ARRAY..GX_LIGHT_ARRAY carry — those are host-endian by construction.
   The shim now tags matrix/light arrays le=true.
4. **Diagnostics** (permanent, env-gated): SB_DRAW_STATS (per-drain draw/vert/byte
   tally), SB_DRAW_DUMP (one-shot per-draw identity: prim/verts/tex0/zmode/translation/
   projection/blend).

## Diagnosis trail (for the next arc)

- SB_DRAW_STATS: steady title frame = ~157 draws / ~1650 verts / ~234 KB fifo — scene
  content IS submitted (perform lists populated: GX=61, GXPost=96 via SB_J3D_DBG; 
  J3DModel::entry() and TDrawBufObj::perform draw() both run constantly).
- SB_DRAW_DUMP census: 95 ortho + 105 perspective draws; perspective draws carry sane
  view-space translations and z-test on. The far-translation (~(79418,164342,-170835))
  draws are the sun/glare billboards.
- STILL MISSING vs GC title: the sky.bmd DOME (752 verts) + cloud strip + sea. Those
  live in `DrawBuf AfterIndirect Xlu` (memory [[sky-bmd-shape-inventory-2026-07-03]]) —
  the INDIRECT phase draw buffers. Next arc: trace why the AfterIndirect buffer's
  packets don't reach the fifo (buffer never drawn? entry filtered? indirect-phase
  EFB-copy dependency?). The J2D letter-quad rectangles visible during the white
  fade-out (frame ~3000) are a separate small blend/TEV artifact to look at.

## Traps hit

- Executable-level no-op stubs silently shadow library implementations — grep
  sdk_stubs.cpp before implementing anything in aurora.
- `strings <object> | grep <new-literal>` is the fast truth test for "did my change
  actually compile in" (one make invocation reported success while skipping a failed
  TU whose error was hidden by an output grep).
- The swapchain/present-source dump is BGRA; `rgba:` conversion produces a convincing
  wrong-colors red herring.

## Continuation (same day, session 2): the scene is EFB-copy-discarded — GXGetTexObjAll

Diagnosis chain, each step falsifying the previous hypothesis:
1. TSmJ3DScn::perform is NEVER called (bit 8 or otherwise) — the scene-draw flow is NOT
   TSmJ3DScn; entry goes through actor perform(0x200) into the named DrawBuf buffers and
   'Draw Buffer Group' (34 DrawBuf children) draws them with bit 8 from unk40. All of
   that runs. sb_boot_drive_scene (deleted Path-B hand-driver) is NOT needed for this.
2. The scene IS therefore in the fifo (the ~105 perspective draws). It disappears because
   SMS renders scene -> TEfbCtrlTex(通常シーン描画ステージ, in PerformList GX Post,
   filter 0x8) copies the EFB into the screen texture WITH CLEAR -> the 2D pass draws on
   the cleared EFB. Faithful GC pipelining.
3. The copy NEVER RAN: TEfbCtrlTex::setTexAttb extracts its destination pointer via
   GXGetTexObjAll — which was an all-zeros stub (inherited from sdk_gap_stubs) → mImagePtr
   null → GXCopyTex skipped → scene render clear-discarded every frame; the uniform
   background is the copy-clear color, not a missing draw.

Fixes landed (aurora cf85ded): real GXGetTexObjAll; resolve_sampled_textures re-resolves a
bind when an EFB copy to its data pointer appears after a stale static resolve; SB_COPY_DBG
tracing. Verified: 2 copies/frame now execute at title (512x549 fmt5 clear=1 + 640x480
fmt4 clear=0).

## Continuation 3: copy IS sampled; EFB persistence landed; scene draws still empty

- [copy-bind] fires 72×: the screen-texture quads DO sample the copy handles (dest
  0x..a020 sampled as a 256x256 texobj, 0x..47860 as 320x224 — dims mismatch the copy's
  512x549/640x480, worth revisiting, but sampling works).
- Landed aurora 8eabe8d: GC-faithful EFB PERSISTENCE — the per-frame first render pass no
  longer clears; only explicit copy-clear erases the EFB (GC semantics; SMS pipelines the
  scene copy across frames). Correct regardless, but did NOT surface the backdrop.
- FALSIFIED by that: the invisibility is not (only) copy/persistence ordering. The
  presented frame contains copy-clear -> late scene-buffer draws -> present, so visible
  scene pixels should not even need the quad — yet background stays at the copy-clear
  color. Strong inference: the ~105 perspective draws in the fifo are the 3D LOGO LETTER
  models (which ARE visible), and the map/sky/sea buffers draw EMPTY.

## NEXT (named): why are the Sky/Map J3DDrawBuffers empty at draw time?

Probe: per-buffer packet count in J3DDrawBuffer::draw (buffer->name registry already
exists for SB_ENTRY_MAT in TDrawBufObj). If Sky/Map buffers show 0 packets while entry()
runs 40 models/frame, the entry lands in the WRONG buffer instances (j3dSys draw-buffer
pointer at actor-entry time) — chase who sets j3dSys.setDrawBuffer during the ENTRY pass
(TDrawBufObj 0x400 setBuf entries in the preEntry list order). Also open: copy texobj
dims vs copy dims mismatch (256x256 texobj / 512x549 copy — GXGetNumXfbLines/
GXGetYScaleFactor still stubbed zero/1.0); J2D letter-quad blend artifact during fades;
PRESS START prompt absent.

## Continuation 4: scene DRAWS pixels; the copy/repaint loop is the remaining defect

- SB_DRAWBUF_STATS (new, J3DDrawBuffer::draw): at title the buffers are FILLED and DRAWN —
  Sky Xlu=6 packets, MapOpa=7, MapXlu=2, Mirror Opa=14, LensFlare=11. Entry/dispatch is
  NOT the problem (falsifies "buffers empty" hypothesis).
- SB_NO_COPY_CLEAR (new, aurora copy_tex diagnostic): with the copy's EFB clear
  suppressed, dark-blue sky-sphere segments and glow accumulation SURVIVE to present —
  scene draws DO emit pixels (scratch/screenshots/title_noclear.png).
- Net: scene renders (at least sky backdrop pieces) → mid-frame copy erases the EFB →
  the screen-texture quad that must REPAINT the copied scene paints nothing visible.
  Narrowed defect: the repaint quad. Known concrete oddity to chase first: the quad's
  texobj is 256x256 / 320x224 while the copies are 512x549 / 640x480 (copy dims flow
  through GXGetNumXfbLines/GXGetYScaleFactor — STILL zero/1.0 stubs in
  sms-boot/runtime/sdk_stubs.cpp; aurora has TODOs). Implement both with real GC
  formulas, re-measure, then inspect the quad's TEV/blend/alpha if still black.
- MapOpa's 7 packets drawing only sky-colored output also suggests map materials render
  dark (lighting/TEV) — verify once the repaint loop shows anything at all.
