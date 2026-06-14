# SMS → native PC port: the plan to remove ALL emulation (master plan + handoff)

**Written 2026-06-14. READ THIS FIRST.** Supersedes the GPU carve-out in
`docs/dolphin_independence.md` (§"GPU/GX→Vulkan: KEEP") — the user has reversed that: the renderer
is now in scope too. North star, in the user's words:

> "Keep porting SMS to PC until it doesn't need any emulation. Build PC-native solutions, don't use
> Dolphin. Start somewhere, keep working until we can remove Dolphin altogether."

This document is the staged plan to get there. It is a living tracker — each stage moves rightward
only with cited verification (oracle A/B, frame dumps, harness verdicts), never by vibe.

---

## 0. Definition of done — and what "no emulation" means

**Done = a single native PC binary that links NO Dolphin at runtime.** It runs the statically
recompiled SMS game code plus a native PC implementation of every subsystem the game touches
(rendering, audio, OS/threading, timing, file I/O, input). Dolphin survives only as an **offline
debugging oracle** (a separate build, never linked into the shipping binary) for the differential
method (`SUNBRIGHT_DISABLE_RECOMP`, the DIFF harness).

**Recompiled game code is NOT "emulation" — it is the port of the game logic.** Static recompilation
translates SMS's own PowerPC into native x86 ahead of time; there is no runtime interpreter for
recompiled functions. Hand-porting all ~9,700 functions is infeasible and pointless; the recompiler
IS the port for game/actor logic. We hand-port *engine layers* (render, audio, OS, timing) where PC
reality demands it (aspect, host clock, host threads, a real GPU API). This is the established N64/PC
static-recomp port model.

**The "emulation" we must delete is Dolphin's hardware emulation:** the GameCube GPU (VideoCommon +
Vulkan backend), the DSP microcode, the MMIO device handlers, CoreTiming, the MMU/address
translation, and the **interpreter fallback** (the one piece of runtime CPU emulation that remains —
used today for JIT-only HW PowerPC ops: `mtmsr`/`rfi`/MMU/HW-SPR).

---

## 1. Current Dolphin dependency surface (what must be removed)

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

## 2. Guiding principles (do not violate)

- **Port behavior, not hardware** (`port-not-emulate`). A native subsystem replicates what the game
  *observes*, not the chip. Don't build "our GameCube GPU"; build the renderer the game needs.
- **Incremental + always-shippable.** Each subsystem is removed independently and verified against the
  Dolphin oracle (`SUNBRIGHT_DISABLE_RECOMP`) BEFORE unplugging it. Never lose correctness to gain
  independence. Keep an A/B off-switch per subsystem during its bring-up; delete the switch once
  verified (no permanent dual paths — `done-right-over-working`).
- **No bandaids.** Root-cause every divergence; the fix is the native port, not a constant/skip.
- **Verification is a first-class deliverable, not an afterthought.** See §5 — the headless
  rendering-verification gap is the single biggest process risk and must be closed before the renderer
  work, or it will burn sessions guessing (it already has).
- **Build tools/diagnostics alongside each subsystem** (env-gated, durable, driveable over the probe).

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

## 6. Recommended order (each stage shippable; dependencies noted)

1. **N0** deterministic render verification (unblocks everything render).
2. **N1** native GC asset decoders — textures + BMD geometry (pure, offline-testable; no GPU).
3. **Native DVD/asset loading** (S, independent, removes a device + latency coupling) — parallel.
4. **N2** native Vulkan backend bring-up (first native pixel: a textured quad).
5. **N3** vertical slice: J2D HUD native (smallest complete asset→object→GPU→present path).
6. **N4** J3D opaque geometry native (vertex pipeline + XF; interp60 matrix seam goes native here).
7. **Native time/event model** + **MMIO devices native** (VI/PE/CP pair with the renderer) — interleave.
8. **N5** TEV→shader (the long pole).
9. **N6 → N7** EFB/screen-space effects + particles native (resolves the interp60 effect-jitter class).
10. **DSP/audio finish** (drop ZeldaAudioRenderer dependency).
11. **CPU: model the remaining JIT-only HW ops natively** (`mtmsr`/`rfi`/MMU/HW-SPR), shrink interp
    fallback to zero.
12. **Boot fully native** (fastboot-only path).
13. **N8 + unlink Dolphin** — delete VideoCommon/backend/DSP/CoreTiming/MMU from the link. Done.

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
  14/14 (tri counts, index winding, attribute interleave, multi-primitive append base). **Next: the
  live J3DShape hook — RE J3DShape/BMD SHP1+VTX1, resolve guest CP ARRAY_BASE/STRIDE → NgxArrays,
  walk a shape's display-list packets → ngx_assemble_primitive → one native mesh per shape.**

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
