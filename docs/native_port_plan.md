# Renderer engineering reference (native GX → Vulkan)

> **This is the live frontier of the project** (architecture direction in `CLAUDE.md` /
> `docs/DO_NOT_REVISIT_FLIP.md`): a native PC renderer that reads engine objects straight from
> **guest-layout RAM** (object-model, not GX-byte/FIFO emulation) and draws them with our own
> Vulkan, no Dolphin. Contents: the stage breakdown (N0–N8), the GX/VAT vertex-decode spec (§3a),
> the J3DShape draw-path RE, verification notes, and the progress log. The "render over
> recomp-on-guest-memory" form is NOT transitional — it IS the target (engine objects stay
> GC-layout; gameplay stays recompiled on the same memory). The earlier "subordinate to
> ARCHITECTURE_TARGET / draws host objects" framing was part of the retired flip direction.

## 1. Dolphin dependency surface (what the renderer/platform must replace)

Status as of 2026-06-14 (from CLAUDE.md, `docs/port_roadmap.md`, session memory):

| Subsystem | Today | Effort to own | Notes |
|---|---|---|---|
| **GPU / rendering** | Dolphin VideoCommon (OpcodeDecoder, VertexLoader, TextureCache, TEV→shader, EFB, Vulkan backend, Present). We own only the GX *frontend capture* (`gx_stream.cpp`, synchronous decode, no CP ring) + the interp60 layer over it. | **XL (dominant)** | The whole renderer. Detailed in §3. |
| **DSP / audio microcode** | Native JAS engine owns SE+BGM synth/mix (`native_jas.cpp`, M1–M3). But the DSP *ucode* math still routes through Dolphin's `ZeldaAudioRenderer` (`zelda_ucode_native.cpp` reuses it), and the AID/mail chain straddles Dolphin DSP. | **M** | Finish: native DAC mix, drop the ZeldaAudioRenderer dependency. |
| **MMIO devices** (VI/PE/CP/SI/EXI/PI/DSP regs) | Routed through Dolphin `MMU::Read/Write<T>` + Dolphin device handlers. Some already native: CARD (`native_card.cpp`), drawsync (`sms_drawsync_lossproof.cpp`), AID (`aid_native.cpp`), parts of VI/CP service in `poll_yield`. | **L** | Each device → native model. Tightly coupled to the renderer (VI/PE/CP) and timing. |
| **CoreTiming / time base** | Hybrid: our native host-clock/audio governor (`sb_time_ahead`) drives a still-present Dolphin `CoreTiming`. `mftb` reads Dolphin's fake TB. | **M** | Native time/event model so recomp credits its own time; precondition for owning devices cleanly. |
| **Memory / MMU** | Fast RAM path is ours (`intrinsics.h` `sb_r*/sb_w*`). MMIO + address translation (BAT/segments, real-mode reads) via Dolphin MMU. | **M** | RAM done. MMIO translation goes away as devices become native. |
| **CPU interp fallback** | Dolphin interpreter runs JIT-only HW funcs (`mtmsr`/`rfi`/MMU/`mfspr`/`mtspr` to HW SPRs) + boot handoff + a few spots. `function_needs_jit()` routes them. | **M** | The last runtime CPU emulation. Model these ops natively in the recompiler (MSR/SRR/BAT state machine) OR make the HW they poke native so the side effects are ours. Shrink toward zero. |
| **OS / threading / IRQ** | **Native-owned** (`nthr` scheduler, native IRQ dispatch, OSLoadContext handoff, native OSCreate/Resume/Sleep). | done (maintain) | Keep; revisit any residual interp of OS primitives. |
| **DVD / asset loading** | Dolphin DVD thread + emulated seek (band-aided `FastDiscSpeed`). ROM data already decoded natively for audio (`tools/jingle`, FST extractor). | **S (do early)** | Serve JKR/DVD read API from extracted files natively, instant. Removes a device + latency coupling. |
| **EXI memcard / SI input** | Memcard native (`native_card.cpp`). Input native (override). | done | — |
| **Boot / HW init** | `SUNBRIGHT_FASTBOOT` is a native port of the boot→gameplay sequencing; the full cold boot still leans on Dolphin HW init. | **M (late)** | Make fastboot the only path, fully native; delete the emulated cold-boot dependency. |

---

## 3. The renderer plan (the dominant effort) — OBJECT-MODEL native renderer

### ⛔ DIRECTION CORRECTION (2026-06-14, user) — supersedes the old "GX-level" decision
> "GameCube GP FIFO decoder is still emulation. I want you to port the game to PC as PC native, not
> just decouple it from Dolphin." … (seam question) → **"PC native PC game."**

The earlier plan chose a **GX-level** renderer: own the GX command stream + register state → native
GPU (and R1 built a Dolphin-free GP-FIFO decoder, commit 102912f). **The user has rejected that as
still-emulation:** decoding the live GP-FIFO byte stream at the gather-pipe / CommandProcessor
boundary is *reimplementing the GameCube's command processor* — decoupled from Dolphin, but still
emulating the GPU pipeline. Removing Dolphin is NOT the goal; a **native PC game** is.

**New seam = the J3D/J2D object model + GC asset formats.** We own, as native PC C++ talking to a
native GPU API:
- the **scene-graph draws** — J3DModel / J3DShape / J3DMaterial, J2DScreen / J2DPane / J2DPicture,
  JPA particles (the game's render objects),
- the **GC asset formats** decoded to native data — **BMD** geometry → native meshes, **BTI/TLUT**
  textures → native textures, **TEV materials** → native shaders,
- a **native GPU backend** (our own Vulkan device/pipeline/present).

The **recompiled PowerPC keeps doing game logic only** — transforms, animation (J3DAnm), collision,
camera, AI, physics — and calls *into our native renderer* instead of the GX SDK. No console GPU
pipeline anywhere: not Dolphin's VideoCommon, not a GX-FIFO reimplementation.

**R1 is repurposed, not wasted.** Pre-baked model geometry (BMD shape packets) IS GX-display-list
*data*; decoding it is **asset decoding**, done offline / at load time into native meshes — NOT a live
runtime FIFO. The R1 framing/vertex-size logic (§3a) becomes that asset-geometry decoder. The runtime
GP-FIFO path + Dolphin VideoCommon get deleted.

### What we port from (faithful RE source)
The full JSystem decomp is vendored at `reference/sms` — port from it directly, don't re-derive:
- J2D: `reference/sms/src/JSystem/J2D/{J2DScreen,J2DPane,J2DPicture,J2DGrafContext,J2DTextBox,J2DWindow,J2DPrint}.cpp`
- J3D: `reference/sms/src/JSystem/J3D/J3DGraphBase/{J3DShape,J3DMaterial,J3DVertex,J3DTevs,J3DTransform,J3DDrawBuffer,J3DPacket,J3DSys}.cpp`
  + `J3DGraphLoader` (BMD load) + `J3DGraphAnimator` (anim).
- Textures: `reference/sms/src/JSystem/JUtility/JUTTexture.cpp`, `J3D/J3DGraphBase/J3DTexture`.
- Particles: `reference/sms/src/JSystem/JParticle/`.
Dolphin's `TextureDecoder` / `PixelShaderGen` are references for *the math* (re-derive natively, don't
link). Validate against the **oracle** (`SUNBRIGHT_DISABLE_RECOMP`) and against Dolphin's frame dumps.

### Native-renderer stages (thin vertical slice first, each independently verifiable)
The strategy is a **thin vertical slice** — get ONE simple thing fully native end-to-end (asset →
object → native GPU → present), then widen — rather than building each horizontal layer in isolation.

**N0. Deterministic headless render verification (PREREQUISITE — still build first).** Frame-count-
driven capture (not wall-ms), dump to disk without the `/verify` readback perturbing pacing, so A/B
compares identical scenes (see `docs/interp60_efb_handoff.md` "VERIFICATION IS BROKEN HEADLESS").
Reuse `tools/interp/verify_*.py`. Without it every render change is an unverifiable guess.

**N1. Native GC asset decoders (pure, offline-testable — START HERE).** No GPU, no running game.
  - **Textures:** GC image formats (I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/CMPR/C4/C8/C14X2 + TLUTs) →
    RGBA8, ported native. Validate against Dolphin's `TextureDecoder` (oracle for the math) over a
    table of formats/sizes. Deliverable: `runtime/render/tex_decode.{h,cpp}`.
  - **BMD geometry:** repurpose R1 (§3a) as an asset display-list → native mesh (positions/normals/
    UVs/colors, indexed) decoder. Validate vertex counts/positions against the live game's draws.

**N2. Native GPU backend bring-up (own Vulkan).** Our own Vulkan device/pipeline/present (reuse the
existing surface during bring-up; we already drive present timing via `sb_present_xfb`). First visible
milestone: a cleared color + one textured quad from a decoded BTI, A/B alongside Dolphin.

**N3. Vertical slice — J2D HUD native.** Override `J2DScreen::draw` / pane draws (J2DScreen::draw @
`0x802cfda8` already located); walk the J2D pane tree from the game's live objects; render native
textured quads (ortho) with decoded textures. Smallest *complete* slice (2D quads, simple blend, no
skinning/heavy TEV). Verify the HUD vs the oracle. This proves the whole pipeline thin.

**N4. J3D opaque geometry.** Override the J3D shape/packet draw; native vertex assembly from decoded
BMD meshes; XF transform (position/normal matrices — the interp60 indexed-matrix seam, array 12 pos /
13 nrm, becomes a native first-class feature here). Render opaque world geometry; A/B vs oracle. The
recompiled J3DModel/anim still computes the matrices.

**N5. TEV → fragment shader (the hard core).** Translate J3DMaterial's TEV combiner + alpha/z/blend
into native SPIR-V; TEV-state→shader cache. Validate per-material vs oracle frame dumps. (`J3DTevs.cpp`
+ Dolphin `PixelShaderGen` as math references.)

**N6. EFB + screen-space effects native.** Our own EFB render target + EFB→texture/XFB resolves.
**This dissolves the interp60 effect class by construction** (water reflection / Mario ghost "frozen
at tick N"): WE own per-field effect textures and re-issue each field's effect at the interpolated
camera — no Dolphin texture-cache aliasing (see §4).

**N7. Particles (JPA), then the long tail** — remaining draw paths, fog, decals, projected shadows.

**N8. Delete Dolphin VideoCommon + backend (and the runtime GP-FIFO path) from the link.** Renderer
fully native.

### 3a. GX display-list / asset-geometry decode spec (was the R1 GP-FIFO spec — now the BMD/DL asset decoder reference)

GP FIFO command framing (opcode = first byte):
- `0x00` NOP — 0 operands.
- `0x08` LOAD_CP_REG — `u8 reg, u32 value`. (reg `0xA0–0xAF` = array base, `0xB0–0xBF` = array stride,
  `0x50` = VCD lo, `0x60` = VCD hi, `0x70+vat` = VAT_A, `0x80+vat` = VAT_B, `0x90+vat` = VAT_C.)
- `0x10` LOAD_XF_REG — `u32 cmd2`; `transfer_size = ((cmd2>>16)&0xF)+1`, `base=cmd2&0xFFFF`; then
  `transfer_size*4` data bytes.
- `0x20/0x28/0x30/0x38` LOAD_INDX_A/B/C/D — `u32 v`; `index=v>>16`, `addr=v&0xFFF`,
  `size=((v>>12)&0xF)+1`. (A→geom/pos-mtx array, B→normal-mtx, C→tex-mtx, D→light.)
- `0x40` CALL_DL — `u32 addr, u32 size` (size in bytes; the referenced bytes are another GX stream —
  recurse/inline).
- `0x44` (unknown metrics) — 1 byte, no operands (stream stays in sync).
- `0x48` INVAL_VC (invalidate vertex cache) — 1 byte, no operands.
- `0x61` LOAD_BP_REG — `u32 v`; reg = `v>>24`, value = `v&0xFFFFFF`. (PE token = BP reg `0x47`;
  PE token-int = `0x48`; EFB copy trigger = `0x52` `PE_COPY_EXECUTE` — confirm against `BPMemory.h`.)
- `0x80–0xBF` DRAW PRIMITIVE — `prim = opcode & 0xF8`, `vat = opcode & 0x07`; then `u16 vertex_count`,
  then `vertex_count * vertex_size(vat)` bytes. (prim types: 0x80 Quads, 0x88 Quads2(unused),
  0x90 Tris, 0x98 TriStrip, 0xA0 TriFan, 0xA8 Lines, 0xB0 LineStrip, 0xB8 Points.)
- else → unknown (framing failure; stop, report offset+opcode).

**Vertex size** = sum over attributes, in this order, using VCD (descriptor) for present/format-class
and VAT for direct-component sizes:
1. PosMatIdx (VCD.low bit 0): present → 1 byte.
2. Tex0..7 MatIdx (VCD.low bits 1..8): each present → 1 byte.
3. Position (VCD.low bits 9-10): Not(0)/Direct(1)/Index8(2)/Index16(3).
4. Normal (VCD.low bits 11-12).
5. Color0 (bits 13-14), Color1 (bits 15-16).
6. Tex0..7 coord (VCD.high bits 0-1,2-3,…,14-15).

Per-attribute byte size (VCF = the 2-bit VCD class; Direct sizes from VAT):
- **Index8 → 1, Index16 → 2** for ALL attributes (pos/nrm/col/tex), EXCEPT the normal Index+Index3
  special case below. NotPresent → 0.
- **Position Direct:** `elemSize(PosFormat) * (PosElements==XYZ?3:2)`. elemSize: UByte/Byte=1,
  UShort/Short=2, Float(+invalid 5/6/7)=4. (Table: Direct UByte/Byte {2,3}, UShort/Short {4,6},
  Float {8,12} for {XY,XYZ}.)
- **Normal Direct:** N → `elemSize*3`; NTB → `elemSize*9`. (Byte→{3,9}, Short→{6,18}, Float→{12,36}.)
  **Normal indexed + NormalIndex3 (VAT_A bit 31) + NTB:** Index8 → 3, Index16 → 6 (three indices,
  one per N/T/B). Without index3, or with N (not NTB): Index8 → 1, Index16 → 2.
- **Color Direct:** by ColorFormat — RGB565=2, RGB888=3, RGB888x=4, RGBA4444=2, RGBA6666=3,
  RGBA8888=4. (Color component-count bit selects the *format* enum used; the size table is indexed by
  format directly: Direct {2,3,4,2,3,4}.)
- **TexCoord Direct:** `elemSize(fmt) * (ST?2:1)`. (Byte→{1,2}, Short→{2,4}, Float→{4,8} for {S,ST}.)

VCD/VAT bit layout (from `externals/dolphin/Source/Core/VideoCommon/CPMemory.h`):
- VCD low: PosMatIdx@0, Tex[i]MatIdx@1..8, Position@9(2b), Normal@11(2b), Color0@13(2b), Color1@15(2b).
- VCD high: Tex[i]Coord @ 2*i (2b each), i=0..7.
- VAT_A (0x70): PosElements@0(1b), PosFormat@1(3b), PosFrac@4(5b), NormalElements@9(1b),
  NormalFormat@10(3b), Color0Elements@13, Color0Comp@14(3b), Color1Elements@17, Color1Comp@18(3b),
  Tex0CoordElements@21, Tex0CoordFormat@22(3b), Tex0Frac@25(5b), ByteDequant@30, NormalIndex3@31.
- VAT_B (0x80): Tex1@0.., Tex2@9.., Tex3@18.., Tex4@27 (split) … ; VAT_C (0x90): Tex5@5, Tex6@14,
  Tex7@23. (Each Tex group: Elements@k(1b), Format@k+1(3b), Frac@k+4(5b).)
ComponentFormat enum: UByte0,Byte1,UShort2,Short3,Float4 (5/6/7 behave as float, size 4).
VertexComponentFormat enum: NotPresent0,Direct1,Index8(2),Index16(3).

Validate every computed vertex size against Dolphin's `VertexLoaderBase::GetVertexSize` on real
primitives during bring-up (differential); any mismatch points at the exact VAT config to fix.

---

## 4. How this resolves the open interp60 / effects bugs (free wins downstream)

The 60fps interpolation work hit a wall that is really a *Dolphin-ownership* wall (see
`docs/interp60_efb_handoff.md`, `docs/re_notes/water_refraction_projection.md`):
- The raw-GX replay (over Dolphin) **freezes screen-space effects at tick N** — the water reflection /
  Mario's ghost swim because their eye-space quad + texgen are baked at N and Dolphin's address-keyed
  EFB-copy cache can't be per-field-addressed cleanly. We patched the water by re-deriving it via the
  guest path at N½ (commit d26ac3a, helped) but Mario's ghost and the general class remain.
- **Once we own the renderer (R6/R7), this class dissolves:** WE allocate per-field EFB-copy textures
  and WE re-issue each field's effect at the interpolated camera, by construction — no Dolphin
  texture-cache aliasing, no "frozen at tick N", no guest re-issue hacks. interp60 stops being "a
  layer over Dolphin" and becomes a native renderer feature. So the renderer port is also the real fix
  for the effect-jitter frontier; do not keep band-aiding it on Dolphin in the meantime.

Current interp60 state to carry forward (all committed): unified replay (both presents via one replay
path, real consistent), owned present cadence (0 gameplay doublings/skips), full motion-interp
coverage, native water re-issue at N½. Dead ends recorded: blanket direct-XF interp (breaks HUD).

---

## 5. Verification strategy (close the gap before R-stages)

- **Deterministic headless capture (R0)** — the prerequisite. Frame-count-driven dumps, no
  readback perturbation, so A/B compares identical scenes.
- **Dolphin as oracle** — keep `SUNBRIGHT_DISABLE_RECOMP` and the DIFF harness; for the renderer, A/B
  our native frame vs Dolphin's frame dump of the same captured GX stream (pixel/structural diff).
- **Per-subsystem A/B switch during bring-up**, deleted once verified.
- **The user is the headed ground-truth verifier** for subtle/temporal artifacts (jitter, reflection
  tracking) — headless can't see those. Build captures the user can label; don't claim "fixed" off a
  headless still (that mistake was made this session).
- **NEVER overlap two running instances** (a zombie on probe port 17654 confounds reads).

---

## 6. Renderer progress log + J3DShape draw-path RE

(The stage ORDER lived here; it was trimmed to the progress log below. The
renderer progress log + the J3DShape draw-path RE below are kept as engineering reference.)

**Progress (2026-06-14):** R1 (Dolphin-free GP-FIFO decoder, 102912f) built+verified, then seam
**corrected by user** (§3): GP-FIFO decode is still GPU emulation → R1 repurposed as the BMD asset
decoder; runtime GP-FIFO path on the delete list. Then, on the object-model architecture:
- **N1 textures ✅** (81c9a5b) — `runtime/render/tex_decode.{h,cpp}`, all GC formats → RGBA8,
  byte-parity vs Dolphin's TextureDecoder (119 cases, 0 fail; `/tex`).
- **N2 first native pixels ✅** (3db6c70) — `runtime/render/vk_quad.cpp`, our own Vulkan pipeline +
  embedded SPIR-V renders a textured quad offscreen (reusing Dolphin's VkDevice as bring-up scaffold),
  readback EXACT vs the decoded texels (`/vkquad`; 64×64 4096/4096, stable under gameplay).
- **N3 walk ✅** (22880d7) — `runtime/render/j2d_walk.cpp`, reads the live J2D HUD pane tree from
  guest memory (bounds-checked), canonical J2DScreen::draw tee publishes the root (`/j2d`).
- **N3 native HUD render ✅** (8f5c201) — `runtime/render/j2d_render.cpp` — FIRST GAME CONTENT drawn
  by our renderer: live draw list → `sb_tex_decode` → our Vulkan ortho pipeline (`quad_ortho.vert` +
  push-constant rect, alpha-blended) → offscreen + PPM (`/j2drender`). Verified: 2 HUD bars at
  pixel-exact screen rects (rows 0-70 + 410-480 full, middle empty), invisible/text panes excluded.

- **N3 J2D color/alpha modulation ✅** (2b1ad64) — faithful J2DPicture TEV (texColor×RASC,
  texAlpha×RASA folding mColorAlpha); corner colors from mCornerColor@0x144. Verified: HUD bars now
  (160,160,160) = white×(160/255) over black (translucency applied; was opaque white).
- **N4 vertex extractor — positions ✅** (d6344e0) — `runtime/ngx/ngx_vertex.{h,cpp}`, reads GC
  position attribute VALUES (u8/s8/u16/s16/float, XY/XYZ, PosFrac dequant, Direct/Index8/16) into a
  native float buffer; `/ngxvtx` self-test.
- **N4 vertex extractor — normals/colors/texcoords ✅** (this commit) — extends the extractor to ALL
  vertex attributes: normals (FracAdjust dequant, NTB→N, direct+indexed), colors (all 6 GC formats
  565/888/888x/4444/6666/8888 with Dolphin's exact per-format endianness, →RGBA8), tex coords (all 8
  channels, per-channel Frac dequant). A shared `ngx_attr_offset()` in `ngx_decode` is the single
  source of vertex-layout truth (ngx_vertex_size re-expressed via it); `resolve_attr` unifies
  Direct/Index8/Index16 fetch. `/ngxvtx` self-test 18/18 (multi-attr offsets, every color format,
  normal/tex dequant, all indexed paths). Pure/offline.
- **N4 mesh assembler ✅** (this commit) — `runtime/ngx/ngx_mesh.{h,cpp}`, the layer above the
  extractors: given a GX DRAW primitive (CP descriptor + opcode + vertex block + resolved indexed
  arrays `NgxArrays`), extracts ALL present attributes into interleaved native vertices `NgxVertex`
  and triangulates the GX primitive type (Quads→2 tris, Tris→list, TriStrip→alternating winding,
  TriFan, Lines/Points→none) into a triangle-index list — exactly a native GPU vertex+index buffer.
  Owns no GX framing of its own (caller supplies the primitive); pure/offline. `/ngxmesh` self-test
  14/14 (tri counts, index winding, attribute interleave, multi-primitive append base).
- **N4 display-list mesh builder ✅** (5936699) — `NgxCP` now tracks per-attribute array base+stride
  (CP regs 0xA0-0xAF/0xB0-0xBF); `ngx_walk_stream` walks a GX command stream invoking a per-DRAW
  callback (shares ngx_decode framing; ngx_parse_frame parity unchanged); `ngx_build_mesh` resolves
  arrays via a guest→host resolver and assembles a whole shape display list into one mesh. `/ngxmesh`
  20/20 (added indexed-array dl build + clean −1 on malformed stream).
- **N4 LIVE J3DShape hook ✅** (this commit) — `runtime/overrides/ngx_j3d_shape.cpp`, the first
  GAME 3D WORLD GEOMETRY assembled natively. Tee on `J3DShape::draw` (0x802e0390, gated
  `SUNBRIGHT_NGX_SHAPE=1`, real draw still runs): builds `NgxCP` from the J3D **objects** (object-
  model, NOT GX byte decode) — `GXVtxDescList` (unk2C@0x2C) → VCD, `J3DVertexData` attr-fmt list
  (unk44@0x44 → +0xC) → VAT, GXAttr/CompType/Cnt enums map 1:1 to NgxCP — resolves live arrays
  (j3dSys@0x804045DC unk10C/110/114 pos/nrm/clr0 override + J3DVertexData clr1/tex), walks each
  `mDraws[i]` display list through `ngx_build_mesh`. Verified live (Delfino, fastboot, 75s, no
  crash): 1.2M shape draws, **0 bad-CP / 0 framing failures**, ~470 shapes/frame, sane meshes
  (e.g. 87 verts→49 tris, vstride 6 matching Index16 pos/clr/tex), real world-space positions
  (−974, 1531, −8200), max 15790 verts/shape. `/ngxshape` probe. GOTCHA found+fixed: J3DShapeDraw
  has a **vtable@0** the decomp header omits → real layout vtable@0 / size@4 / list@8 (confirmed by
  ctor+draw disasm); reading size@0/list@4 skipped every list (0 verts).
- **N4 native XF (vertex transform) ✅** (this commit) — the J3D transform stage ported native. The
  shape hook now reads the live modelview `j3dSys.mCurrentDrawMtx` (Mtx* @ j3dSys+0x104, 3×4
  row-major) — the SAME matrix the recompiled J3D computes (the interp60 pos-matrix seam, now
  consumed natively) — and transforms every extracted model-space position to eye space. Capture
  moved to AFTER the super-call: `J3DShape::draw` is what sets the per-view arrays (loadVtxArray) AND
  the matrix (setModelDrawMtx) for THIS shape, so pre-draw capture read stale (previous-shape)
  state. Verified live (Delfino): 167M verts transformed, **98.9% land in front of the camera
  (eye z<0)**, no_mtx=0 — a wrong matrix or garbage positions could never yield 99% coherently
  in-front, so this proves extraction + matrix capture + transform are all faithful. `/ngxshape`
  now reports xf in-front %, eye bbox, sample eye pos.
- **N4 native projection (full vertex pipeline) ✅** (this commit) — closes the transform chain:
  the scene_render GXSetProjection tee (0x80362c34) publishes the authored perspective matrix to
  ngx (`ngx_set_projection`); the shape hook computes clip = P·(eye,1) and NDC = clip.xyz/clip.w
  per vertex. The recompiled game's matrices, consumed natively, now take model→eye→clip→NDC with
  ZERO Dolphin. Verified live (Delfino): **clip.w>0 count == eye z<0 count exactly** (172970307,
  the consistency check — for GX perspective w=−ez so they must match iff the matrix form is right),
  and **57.5% of 175M verts project inside the NDC screen box** (≈99% in front, ≈57% on-screen, rest
  off-screen geometry the game submits — exactly the profile of a real 3D scene). The native vertex
  pipeline is complete bar rasterization.
- **N4 FIRST NATIVE 3D WORLD PIXELS ✅** (this commit) — `runtime/render/vk_mesh.cpp` + mesh
  shaders. Our own Vulkan pipeline (clip-space passthrough vert + flat vertex-color frag, vertex-
  buffer input, 640×448 offscreen) rasterizes the game's REAL J3D geometry — the J3DShape hook now
  snapshots a rolling window of clip-space triangles (clip.xyzw + rgba0, triangle-aligned) which
  `sb_ngx_render` (/ngxrender) draws and reads back. Verified live (Delfino): 10968 tris → **27.7%
  coverage, and the readback PPM is unmistakably Delfino Plaza** (Shine Gate arch + buildings +
  ground features, correct perspective/orientation) — recognizable scene structure proves the whole
  chain (extract→modelview→projection→native raster) is faithful, Dolphin-free. No crash/VK errors.
  Vert shader forces z=0 (GX NDC z isn't in Vulkan's [0,w]) + Y-flip; no depth test yet (painter's
  order).
- **N4 native depth testing ✅** (this commit) — faithful GC→Vulkan depth. GC NDC z ∈ [−1(near),
  0(far)] (confirmed vs Dolphin VertexShaderGen "near z<−w / far z>0") → Vulkan [0,1] via the
  documented map depth=clip.z/w+1 (clip-space z'=clip.z+clip.w; NOT a magic constant), which also
  makes Vulkan's depth-clip do correct near/far frustum clipping. Added a D32 depth attachment +
  depth-test (LESS_OR_EQUAL, write, clear 1.0). Verified live (Delfino, 118805 tris, 100% coverage):
  the readback shows the plaza buildings correctly OCCLUDED behind foreground geometry, foliage in
  front, no z-fighting / no far-over-near — depth-correct. (Exact GXSetViewport depth-range scaling,
  for depth-texture effects, is a later refinement; occlusion is faithful.)
- **N5 first slice — native texture sampling ✅** (this commit) — the bound GX texmap-0 texture now
  textures the geometry, decoded natively. A GXLoadTexObj tee (0x80360160) records the current
  texmap-0 (addr/w/h/fmt from the GXTexObj packed regs: image0@0x08, image3@0x0C); the J3DShape
  capture groups triangles into per-texture BATCHES (NgxRenderBatch + tex0 UV added to the snapshot
  vertex, shared contract in `runtime/ngx/ngx_render_data.h`); vk_mesh decodes each unique texture
  with the N1 decoder (`sb_tex_decode`), uploads it, and draws each batch with its own descriptor
  set; the frag shader does texColor×vertexColor (no/unsupported tex → 1×1 white = flat color).
  Verified live (Delfino): real building window/wall texture detail appears (vs the flat-white
  version), correct UVs, no crash/VK errors. KNOWN-INCOMPLETE (honest): dim, because most SMS
  surfaces use PALETTE textures (C4/C8/C14X2) — skipped here (need the TLUT) → flat dark vertex
  color — and there is no lighting / full TEV combiner yet.
- **N5 — TLUT capture + colorless-default fix ✅** (this commit) — palette (CI) texture support +
  a correctness fix. A GXLoadTlut tee (0x803601fc) records each TMEM tlut slot's palette
  (addr/fmt from __GXTlutObjInt: tlut@0x00 fmt-bits10-11, loadTlut0@0x04 addr>>5); the GXLoadTexObj
  tee resolves a CI texobj's tlutName (texobj+0x18) → palette, carried in NgxRenderBatch; vk_mesh
  feeds it to the N1 decoder (already palette-parity-tested). Bounds-checked (C4=16/C8=256/C14X2=
  16384 entries). Also FIXED: the mesh assembler defaulted absent vertex colors to BLACK → killed
  texture-only/colorless geometry to black; now defaults to WHITE (faithful for the texColor×color
  modulate — texture passes through). Verified no crash/VK errors.
  **HONEST STATE / THE REAL FRONTIER:** the scene is still dim. Diagnosis (not guesswork): the
  earlier flat-color renders (o=vColor) were bright; the ONLY change that darkens is ×texColor → so
  `texColor×vColor` is demonstrably NOT the right combine for these materials. Guessing a combine is
  a bandaid. **The fix is the real per-material TEV combiner — N5 proper: walk J3DMaterial's TEV
  stages (reference/sms J3DTevs.cpp; Dolphin PixelShaderGen as the math reference, re-derived) →
  generate a per-material-state SPIR-V fragment shader (TEV-state→shader cache), with the actual
  Konst/raster/texture inputs + alpha/blend.** Then lighting (normal transform via
  j3dSys.mCurrentNormMtx). Low texture-capture count per chunk also suggests some bindings come via
  GXLoadTexObjPreLoaded / other texmaps — widen capture alongside TEV.
- **N5 proper — per-material TEV combiner ✅** (this commit, 3 steps) — the real GX
  TEV combiner now runs, replacing the guessed `texColor*vColor`:
  1. **Material capture** (`ngx_j3d_shape.cpp`): the live J3DMaterial is found at
     `J3DShape::draw` time via the j3dSys global — `mMatPacket` (j3dSys+0x3C) →
     `J3DMatPacket::unk38` (+0x38) → `J3DMaterial::mTevBlock` (+0x28). The concrete
     J3DTevBlock variant is identified by its vtable (gmse01: TV16 0x803E0A14, TVB4
     0AB0, TVB2 0B4C, TVB1 0BE8 — anchored to TVB1 disassembled from
     `__dt__12J3DTevBlock1`, spacing from the JP symbol map). Per-stage 24-bit
     color/alpha combiner regs (the J3DTevStage 8 bytes ARE the two GX combiner BP
     values), tev order, konst sel, 4 S10 TEV regs + 4 KONST regs are read straight
     from guest RAM (object-model) into `NgxTevState`, deduped by FNV key; each
     `NgxRenderBatch` carries its material index. Verified: found=100% / unknown_vt=0
     over 562787 draws, 78 unique materials, all 4 vtables correctly named (`/ngxshape`).
  2. **TEV→GLSL generator** (`runtime/render/tev_shader.{h,cpp}`) — translates an
     `NgxTevState` to a GLSL 450 fragment shader reproducing the GX integer TEV
     pipeline faithfully: per-stage `d (±) lerp(a,b,c)` with bias/scale/clamp/dest
     (CPREV/C0/C1/C2), the 8 compare modes, konst color/alpha sel, raster + texture
     inputs. Math re-derived from Dolphin PixelShaderGen (not linked). Compiled by our
     OWN glslang wrapper (`runtime/render/glsl_compile.{h,cpp}`, no Dolphin header
     injection — renderer-independent). Verified: `/tevshader` compiles
     GX_MODULATE/REPLACE + every live material, 14/14 0-fail.
  3. **vk_mesh wiring** — one fragment pipeline per distinct material TEV state
     (TEV-state→pipeline cache, slot 0 = modulate fallback), bound per batch; konst
     + c0/c1/c2 regs fed via fragment push constants. Verified headless (Delfino):
     11 material shaders + modulate, 0 fails, 97.8% coverage, recognizable plaza,
     no crash/VK errors over 7936 frames.
- **N6 — colour-channel LIGHTING ✅** (this commit, `ngx_j3d_shape.cpp`) — the TEV
  raster colour is now the real GX lit channel colour, not raw CLR0. The full GC
  per-vertex lighting model is ported natively (`light_vertex`, math re-derived from
  the GC hardware model / Dolphin VertexShaderGen):
  - **Inputs captured object-model / via GX tees** (no byte-stream decode): the
    material's `J3DColorBlock` (variant by vtable — `J3DColorBlockLightOn` 0x803E0CD4,
    `LightOff` 0x803E0D38, both verified by reading the live vtable's `getType` slot
    via the probe) gives the channel-control reg (`mColorChan[0]`: matSrc/enable/
    lightMask/diffuseFn/attnFn/ambSrc) + the per-material material-colour register.
    The 8 hardware lights are captured at `GXLoadLightObjImm` (0x8035f26c) into an
    eye-space table (colour@0x0C, cosAtten@0x10, distAtten@0x1C, pos@0x28, dir@0x34 —
    offsets verified by disasm of GXInitLightPos/Color/Attn/SpecularDir). **The
    ambient is the GLOBAL hardware register** captured at `GXSetChanAmbColor`
    (0x8035f3b4) — a `LightOff` block (the ONLY variant SMS Delfino uses) does NOT
    store/load an ambient, so the per-block read returns 0; the scene sets ambient
    globally. Reading the block's 0-ambient was the "everything dark" bug.
  - **Per-vertex evaluation** in `transform_eye`: normal → eye space via
    `j3dSys.mCurrentNormMtx` (+0x108, Mtx33), position → eye space via the modelview
    (already done); then `illum = ambient + Σ attn·diff·lightColour` over the masked
    lights (NONE/SPEC/SPOT attenuation, NONE/SIGN/CLAMP diffuse), `channelColour =
    matColour · clamp(illum)`. Lighting-off channels reduce to the material source
    (register or vertex colour) → vertex-lit world materials are unchanged.
  - **Verified**: light space is correct — the captured sun position SHIFTS with the
    camera (eye-space). Adding the ambient roughly DOUBLED lit-vertex luminance
    (0.211→0.432, `/ngxshape` `lit_verts mean_lum`). `/ngxrender` still compiles all
    materials, 0 fails, 100% coverage, no crash. Diagnostics live behind
    `SUNBRIGHT_NGX_SHAPE` in `/ngxshape` (per-light col/pos/dir/atten, ambient reg,
    colour-block vtable histogram, lit/flat luminance, mean diffuse, normal-length +
    sun-alignment probes).
  - **OPEN (headed-verification + fidelity, NEXT)**: the headless still is still
    dim/purple-tinted. The lighting MATH + space are faithful, but fidelity vs the
    oracle (is the scene as bright/correctly-coloured) needs the headed user — the
    captured sun colour is sometimes ~0.31 and ambient ~purple; confirm those are the
    real per-draw values (vs a transient global) and that no material double-darkens
    (matSrc=VTX + lighting on a colour that already baked light). Likely the remaining
    darkness is also dominated by the narrow texture coverage below.
  - **Still first-slice gaps** (lower priority): identity TEV swap tables, no alpha
    test (PE block uncaptured → foliage cutouts show full quads), no indirect stages.

- **N7 — native present ✅ (the ngx frame IS the on-screen image)** — `SUNBRIGHT_NGX_PRESENT=1`
  makes the native renderer's output the actual on-screen frame (and the frame dump), replacing
  Dolphin's GX output in the render path. `runtime/render/ngx_present.cpp` is a PERSISTENT renderer
  (vs the one-shot `/ngxrender` probe): shaders are compiled ONCE and pipeline + decoded-texture
  caches survive across frames (per-frame glslang would be unusable). It renders the captured J3D
  scene into a persistent Dolphin `AbstractTexture` each frame on the video thread (its own command
  buffer + fence, isolated from Dolphin's StateTracker, ordered before Dolphin's present submit;
  a per-frame GPU-bubble bring-up cost), then `VKTexture::OverrideImageLayout`. The fork's
  `Present.cpp` substitutes that texture for `m_xfb_entry->texture` at `RenderXFBToScreen`
  (on screen) + `ProcessFrameDumping` (the dump — so it's headless-verifiable) when
  `g_sb_ngx_present`. Verified headless: init_ok, 444 frames, only 92 pipelines built + 187
  textures decoded TOTAL (cached, not per-frame), stable. The dumped frame is the ngx render —
  bright Delfino (meanRGB 200, NO HUD) vs the Dolphin baseline (meanRGB 81, with HUD/subtitle) —
  proving the substitution. NEXT: composite the J2D/HUD overlay into the present (the native frame
  has no HUD yet); cache lifetime/eviction on scene change; lighting/fog fidelity vs the oracle;
  then peel more of Dolphin's VideoCommon.

- **N7-present groundwork ✅ — render into an external color target** — parameterized the
  renderer (`vk_mesh.cpp`): `sb_ngx_render_into(view,img,w,h,final_layout,…)` rasterizes the
  ngx mesh into a caller-supplied color image left in a chosen layout (e.g. SHADER_READ), no
  readback — the primitive the present seam needs to fill the on-screen XFB texture. `/ngxrender`
  keeps its own offscreen RT+readback unchanged. `/ngxpresent` (`sb_ngx_present_test`) verifies
  the primitive headless: renders the scene into an external image, reads it back → bright correct
  Delfino (meanRGB ~197), `scratch/screenshots/ngx_present.png`. The probe self-test uses its own
  raw-Vulkan image/cmd buffer (safe off the video thread); **Dolphin `AbstractTexture` interop
  must run on the video thread** (StateTracker isn't thread-safe). NEXT (the actual present): on
  the video thread (`Presenter::Present` before `BindBackbuffer`, + `ProcessFrameDumping`),
  render into a persistent Dolphin `AbstractTexture` and substitute it for `m_xfb_entry->texture`
  — but record into Dolphin's command buffer (don't do our own submit + `vkDeviceWaitIdle`
  per frame) and persist pipelines/textures across frames; use `VKTexture::OverrideImageLayout`.

- **N7 — PE block: alpha test + blend + z-mode ✅ (foliage/transparency)** — ported the
  J3D pixel-engine block (`J3DMaterial mPEBlock`@+0x30) so cutout foliage and translucent
  surfaces render correctly. Before: trees were solid green blobs and transparent decals
  near the statue showed as opaque black rectangles. After: trees are see-through leafy
  cutouts, the black artifacts are gone, blend applies to xlu surfaces (brightness 192→196,
  coverage 100%, 0 shader fails). Mechanics:
  - **Variant ID self-identifying** — read the block's `getType()` FourCC at runtime, not a
    hardcoded vtable: stored vtable ptr → slot 0 at +8 (CW/GC convention), `getType` is
    virtual slot 2 → vtable+0x10; its body is `lis r3,HI; {ori|addi} r3,r3,LO; blr` →
    decode the 32-bit tag. (Delfino's getType compiles as `lis;addi`, so the decoder must
    accept opcode 14 with sign-extension, not just `ori`/opcode 24.) On Delfino **every**
    material is `'PEFL'` (full block); presets `'PEOP'`/`'PEED'`/`'PEXL'` are ported too
    (GX state verbatim from `J3DPEBlock*::load`, `reference/sms J3DMaterial.cpp`).
  - **PEFL fields** (J3DPEBlockFull): `mAlphaComp`@0x08 (`mAlphaCmpID` u16@0x08, ref0@0x0A,
    ref1@0x0B), `mBlend`@0x0C (`J3DBlendInfo` raw mode/src/dst/logic), `mZMode`@0x10
    (`mZModeID` u16). The alpha-comp / z-mode **IDs decode as a plain bitfield** — that IS
    what `makeAlphaCmpTable`/`makeZModeTable` (`J3DTevs.cpp`) build: alphaID =
    `(comp0<<5)|(op<<3)|comp1`, zID = `(cmpEn<<4)|(func<<1)|updEn`; ID 0xFFFF = "no change"
    (keep default). No need to read the runtime `.bss` table.
  - **Where it lands:** alpha test → baked into the generated TEV fragment shader as
    `discard` after the last stage (compare clamped `prev.a` vs ref0/ref1 combined by the
    GX alpha op AND/OR/XOR/XNOR); blend + z-mode → the Vulkan pipeline (per-material, since
    the pipeline is 1:1 with the TEV-state which now includes `NgxPEState` in its hash).
    GX→VK: blend-factor slots 2/3 are context-named (SRCCLR vs DSTCLR) → Dolphin's
    src/dst tables; `GXCompare` values map 1:1 onto `VkCompareOp` (depth + nothing else).
  - Verified `/ngxshape`: 348k materials all PEFL, 198k alpha-tested, 84k blended, 29k
    z-write-off. NEXT gaps: N7 present (live on-screen path), TEV swap tables, indirect
    stages, fog; transparency draw-order sorting; lighting-fidelity headed check.

- **N6.7 — object-model per-material textures ✅ (THE darkness fix)** — Delfino now
  renders BRIGHT and correctly textured (Shine Gate, sandstone buildings, tiled plaza,
  hills, sky), Dolphin-free. The GX-tee texture path was fundamentally wrong for J3D:
  J3D preloads textures into TMEM upfront, so `GXLoadTexObj`-at-draw is STALE (every
  shape in a frame bound the same one texture → "4 unique textures"). Fix reads the
  binding object-model: material's TEV block `mTexNo[texmap]` (@blk+0x04, u16, count by
  variant) → `j3dSys.mTexture` (J3DSYS+0x54: count u16@0x0, `ResTIMG* mResources`@0x4) →
  `ResTIMG[texNo]` (size 0x20: format@0, width@2, height@4, colorFormat@9,
  paletteOffset@0xC, imageDataOffset@0x1C, both self-relative) → image at
  `timg+imageDataOffset`. Result: 4→159 unique textures, `/ngxrender` mean brightness
  15→192. Confirmed the earlier "dark" was TEXTURES, not lighting/texgen (both A/B-ruled-
  out). The GX tees are retained as `/ngxshape` diagnostics only.
  - NEXT gaps (now that the base is correct): alpha test (PE block → `discard` for
    foliage cutouts), Vulkan blend state (J3DBlend), TEV swap tables, indirect stages,
    fog; then N7 present (make this the live on-screen path); and the lighting fidelity
    headed check (layered correctly now, but confirm vs oracle).

- **N6.5 — per-texmap texture capture + per-stage sampling ✅** (this commit) — the
  producer now captures the binding for ALL 8 GX texmaps (both `GXLoadTexObj`
  0x80360160 and `GXLoadTexObjPreLoaded` 0x8035ffb8 fire for texmaps 0..7 — verified
  by the per-texmap histogram in `/ngxshape`), each `NgxRenderBatch` carries an
  `NgxTexBind[8]`, and the generated TEV shader samples `tex[stage.texmap]` from an
  8-element `sampler2D` array (one descriptor set of 8 per batch in `vk_mesh`).
  Replaces the single-texmap-0 binding. Faithful + no regression (0 fails, 100% cov),
  but **little visible change on the Delfino entry** because the real texture-fidelity
  blocker is now **TEXCOORD / texgen**, NOT texmap coverage:
  - **OPEN — texcoord/texgen (the texture frontier, NOW SCOPED).** TEV stages
    reference a *texcoord* index, but the producer feeds only the vertex's texcoord0
    UV to every stage → multi-texcoord materials sample at the wrong UVs. Measured the
    actual texgen usage on Delfino entry (`/ngxshape` texgen histogram, read from
    `J3DTexGenBlockBasic`: `mTexCoord[8]`@blk+0x8 {type@+0,src@+1,mtx@+2}, `mTexMtx*[8]`
    @+0x28): src is ~80% `GX_TG_TEX0/1/2` (vertex tex attrs 0/1/2), ~18% `GX_TG_COLOR0`
    (+ `GX_TG_SRTG` type), small `NRM`/`POS`; type is mostly `MTX2x4`; and **the texgen
    matrix is NON-identity for ~81% (455k/560k)**. So the dominant case is *vertex tex
    attribute × a per-material texgen matrix* — even texcoord0 (currently passed raw) is
    wrong wherever the SRT/texgen matrix isn't identity. The next stage: capture each
    used texcoord's texgen (type/src/matrix from `mTexMtx[i]` — live `J3DTexMtx`, calc'd
    per frame), compute `uv = TexMtx · sourceInput` per vertex in the producer, pass the
    used texcoords per vertex, and select per stage in the TEV shader. (`GX_TG_COLOR0`/
    `SRTG` + `POS`/`NRM` env/proj mapping are the long tail after the TEX×matrix case.)

**J3DShape DRAW PATH (RE'd 2026-06-14, `reference/sms/.../J3DShape.cpp`) — the live-hook seam.**
`J3DShape::draw()` does, in order:
  1. `GXCallDisplayList(mGDCommands, 0xC0)` — a prebuilt 0xC0-byte GD list that programs the GP-FIFO
     CP state for this shape: `GDSetVtxDescv(unk2C)` (→ VCD), `makeVtxArrayCmd()` (→ `GDSetArray`/
     `GDSetArrayRaw` array bases+strides for attrs POS..TEX7), `J3DSetVtxAttrFmtv(GX_VTXFMT0, …)`
     (→ VAT). So after this call, **our `ngx_cp_state()` VCD/VAT are already correct** if the FIFO
     capture fed them — but the per-attribute **array bases** (CP reg 0xA0+i, strides 0xB0+i) are
     NOT yet tracked by `NgxCP` (only matrix arrays 12/13 are). **TODO: extend `NgxCP` to record
     array base+stride for attrs 0..11**, set in `load_cp` (reg 0xA0–0xAF base, 0xB0–0xBF stride).
  2. `loadVtxArray()` — OVERRIDES the POS/NRM/CLR0 array bases via `J3DLoadArrayBasePtr` to
     `j3dSys.unk10C/110/114` (the live per-view vertex buffers; for skinned models these are the
     CPU-transformed positions, NOT the static BMD arrays). **The native hook must read the array
     base from the LIVE CP state after this, not from the BMD/`unk44` directly**, or skinned meshes
     resolve to the wrong (untransformed) vertices.
  3. matrix load + `setModelDrawMtx`/`setModelNrmMtx` (the pos/nrm matrix the recompiled game already
     computes — interp60 matrix seam), then per element `mMatrices[i]->load()` + `mDraws[i]->draw()`.
  4. `J3DShapeDraw::draw()` = `GXCallDisplayList(mDisplayList, mDisplayListSize)` — **this is the GX
     primitive stream** (BMD SHP1 packet bytes) to walk with ngx framing → `ngx_assemble_primitive`.
Plan for the hook: tee `J3DShapeDraw::draw` (or the shape draw); build `NgxArrays` from the live CP
array base/stride regs (host-resolved); frame `mDisplayList` (extend ngx_decode to emit primitives,
or add a callback) → `ngx_assemble_primitive` per DRAW → one `NgxVertex`+index mesh per shape.
Verify counts/positions vs the oracle. The matrix transform stays recompiled; we own only the draw.

**PRESENT FINDING (2026-06-14):** making the native render visible must NOT be a quick overlay via
`sb_present_xfb`/`Presenter::ViSwap` — that path IS Dolphin VideoCommon (XFB = Dolphin AbstractTexture),
so an overlay would lean *more* on Dolphin (wrong direction). Visible native output = **own the
swapchain** (N7), a real milestone. Until then the native renderer is verified offscreen via readback.

**Next concrete steps:**
1. **N4 J3D world geometry** (the bulk of the game) — object-model: RE J3DShape + BMD SHP1/VTX1, decode
   a shape's display-list packets to a native mesh (positions/normals/uv/color, direct + indexed). The
   GX vertex-attribute extractor is unit-testable against a known oracle (extend the ngx approach).
2. N3 fidelity polish (lower priority, not in current scene): J2DTextBox/font, palette resolution
   (C4/C8/C14X2 — decode already parity-proven; needs JUTPalette→GXTlutObj data ptr), multi-texture.
3. N0 deterministic capture + N7 own-swapchain present (make it all visible + A/B-able).
Still pending/parallel: N0 deterministic capture, native present/swapchain (N7).

---

## 7. Risks / honest unknowns
- **TEV→shader (N5) is genuinely hard** — it is the core of GC rendering. Budget accordingly; use
  Dolphin's `PixelShaderGen` only as a *reference for the math*, re-derived natively.
- **J3D surface is large/less-bounded** — this is the cost of the object-level seam (the GX-level seam
  was rejected as still-emulation). Mitigate with the thin-vertical-slice approach (J2D HUD first) and
  by porting faithfully from `reference/sms` JSystem rather than re-deriving. Keep the recompiled
  J3DModel/anim doing the matrix/animation math; we own only the *draw*.
- **Vulkan device ownership vs Dolphin during bring-up** — running our Vulkan alongside Dolphin's on
  one surface is awkward; may need to take the surface fully (N2) earlier than ideal, or render
  offscreen and blit. Decide at N2.
- **Asset display-list state persistence** — CP/VAT/array state persists across BMD shape packets; the
  asset-geometry decoder (repurposed R1, §3a) must thread it exactly (the analyzer already relies on
  this). This is now a load-time/offline concern, not a live-FIFO one.
- **Scope is months.** The user has explicitly accepted this (`pc-game-architecture-directive`). Keep
  each stage shippable so progress is always demonstrable.
- **Don't lose the recomp.** Game/actor logic stays recompiled; this plan removes Dolphin's *hardware*
  emulation, not the recompiled game code.

---

## 8. Pointers
- Render seam today: `runtime/gx_stream.{h,cpp}` (owned frontend), `runtime/gx_parse.{h,cpp}` (oracle
  parser to validate R1 against), `runtime/overrides/interp_*.cpp` (interp60).
- Effects RE: `docs/re_notes/water_refraction_projection.md`, `efb_dynamic_texture_chain.md`,
  `efb_native_60fps.md`; interp60 handoffs: `docs/interp60_handoff.md`, `docs/interp60_efb_handoff.md`.
- Subsystem tracker: `docs/port_roadmap.md`. Independence rationale: `docs/dolphin_independence.md`
  (note: its GPU carve-out is now superseded by this plan).
- Dolphin fork (we own it): `externals/dolphin`, branch `sunbright` — but the goal is to NOT link it.
