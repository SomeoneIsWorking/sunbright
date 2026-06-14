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

## 3. The renderer plan (the dominant effort) — GX-level native renderer

### Why GX-level, not object-level (J3D)
Two candidate seams were considered:
- **Object-level (intercept J3D/scene draws, render meshes/materials natively, bypass GX).** Rejected
  as the primary: SMS rendering is overwhelmingly **GX display lists** — pre-baked GX command bytes in
  the BMD/asset data — so GX interpretation is unavoidable regardless. And J3D + every actor draw +
  particles + HUD is a far larger, less-bounded surface than the GX API.
- **GX-level (own the GX command stream + GX register state → native GPU).** CHOSEN. The GX SDK
  (~210 functions) and the GX FIFO command set are a **fixed, bounded seam** that ALL rendering passes
  through (immediate mode AND display lists). We already own the frontend capture and have started the
  decoder. The recompiled game keeps calling GX/J3D unchanged; we replace what's *below* GX.

### Renderer sub-stages (each independently verifiable)

**R0. Deterministic headless render verification (PREREQUISITE — build first).**
The current `/verify` readback perturbs timing so captures aren't frame-aligned across runs (see
`docs/interp60_efb_handoff.md` "VERIFICATION IS BROKEN HEADLESS"). Before touching the renderer, build
a deterministic capture: drive a fixed number of GAME FRAMES (not wall-ms), dump each to disk without
the readback perturbing pacing, so an A/B compares identical scenes. Without this, every renderer
change is an unverifiable guess (it has already cost ~2 sessions). Reuse `tools/interp/verify_*.py`.

**R1. Own GX command decoder — ✅ DONE + VERIFIED (commit 102912f).** Our own GameCube GP FIFO
decoder, NO Dolphin `OpcodeDecoder`/`VertexLoaderBase`. Pure logic, tested headless by parity against
the Dolphin-based `gxp_parse_frame` (oracle). Command framing + persistent CP state (VCD/VAT/array
bases) + vertex-size math, all in `runtime/ngx/ngx_decode.{h,cpp}` (constants transcribed from
externals/dolphin VideoCommon; vertex sizes via formula, DL counted-not-recursed to match the
analyzer). Parity harness: `SUNBRIGHT_NGX_PARITY=1` runs both in lockstep each frame boundary
(gx_stream.cpp), compares prims/DLs/copies/token-offsets/matrix-array-offsets+values/ok/fail-offset;
read via the `/ngx` probe endpoint or the `SUNBRIGHT_DBG_GXS` line. **Verified:** fastboot → Delfino
Plaza, walk+jump+heavy camera — 1261 frames mismatch=0 (run 1); `parse ok=384 fail=0` (all full
parses, no trivial fail-matching), `ngx cmp=384 mismatch=0` over 114 MB decoded stream (~310 KB
frames), verdict PARITY-OK (run 2). The exact spec is preserved in §3a below for reference.

**R2. GX register state model.** Mirror the GX state the decoder sees (BP/CP/XF registers) into our own
state structs (TEV stages, tev/alpha/z config, tex coord gen, viewport, scissor, blend, format). This
is the input to the shader translator and the fixed-function setup. Validate field-by-field against
Dolphin's `bpmem`/`xfmem`/`cpmem` on real frames.

**R3. Native GPU backend bring-up (own Vulkan, in parallel with Dolphin).** Stand up our own Vulkan
device/swapchain (or share the surface) and a minimal pipeline. First visible milestone: present a
cleared color + a single hardcoded triangle from our path, A/B alongside Dolphin. This is where the
"own the present/scan-out" peel lands (we already drive present timing via `sb_present_xfb`).

**R4. Vertex pipeline.** Decoder draws → our vertex assembly (CP/VAT formats → native vertex buffers)
→ XF transform (position/normal matrices, the indexed matrix arrays the interp60 work already
understands — array 12 pos, 13 nrm) → a vertex shader. Render opaque geometry natively; A/B vs
Dolphin. The interp60 motion-interp seam (`sb_slot_xf_indexed`) becomes a native, first-class feature
here instead of a hook over Dolphin.

**R5. TEV → fragment shader (the hard core).** Translate the GameCube TEV combiner + alpha/z/blend
state into our own SPIR-V/shaders. This is the single hardest piece of GC rendering. Build a TEV-state
→ shader cache. Validate per-material against Dolphin frame dumps. (Dolphin's `PixelShaderGen` is the
reference for *what* the math is — re-derive it natively, don't link it.)

**R6. Textures + EFB.** GC texture format decode (CMPR/I4/I8/IA/RGB565/RGB5A3/RGBA8/C4/C8/C14X2 +
TLUTs) → native textures; sampler state. EFB as our own render target; EFB copies (to texture / to XFB)
as our own resolves. **This is where the interp60 screen-space effect problems dissolve by
construction:** when WE own the EFB-copy textures per field, the water reflection / Mario's ghost
"frozen at tick N" class disappears — each field's effect is our own correctly-addressed copy (see §4).

**R7. Framebuffer / XFB / present, fully native.** EFB→XFB resolve, XFB→swapchain present, all ours.
Delete the Dolphin Present/VideoInterface path and the `sb_present_xfb` shim into Dolphin.

**R8. Delete Dolphin VideoCommon + backend from the link.** Renderer fully native.

### 3a. GX decoder spec (gathered this session — implement R1 directly from this)

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

1. **R0** deterministic render verification (unblocks everything render).
2. **Native DVD/asset loading** (S, independent, removes a device + latency coupling) — can run in
   parallel with R0/R1.
3. **R1 → R2** GX decoder + state model (pure logic, headless-testable; no GPU yet).
4. **Native time/event model** (enabler for owning MMIO devices cleanly) — can interleave.
5. **R3 → R4** native Vulkan bring-up + vertex pipeline (first native pixels).
6. **MMIO devices native** (VI/PE/CP first — they pair with the renderer; SI/EXI/PI follow).
7. **R5** TEV→shader (the long pole).
8. **R6 → R7** textures/EFB + framebuffer/present native (resolves the effect-jitter class).
9. **DSP/audio finish** (drop ZeldaAudioRenderer dependency).
10. **CPU: model the remaining JIT-only HW ops natively** (`mtmsr`/`rfi`/MMU/HW-SPR), shrink interp
    fallback to zero.
11. **Boot fully native** (fastboot-only path).
12. **R8 + unlink Dolphin** — delete VideoCommon/backend/DSP/CoreTiming/MMU from the link. Done.

**Progress:** R1 ✅ DONE+VERIFIED (commit 102912f — native GX decoder at byte-parity vs the oracle
over a 114 MB gameplay capture, 0 mismatches). **Next concrete steps:** R0 (deterministic
frame-count-driven capture, still the prerequisite for any *pixel*-level renderer verification) and
R2 (mirror the GX register state — BP/CP/XF — into our own structs, validated field-by-field against
Dolphin's bpmem/xfmem/cpmem; the decoder from R1 already gives us the command/CP-state seam to build
on). Both are pure, headless-verifiable, Dolphin-free foundation work.

---

## 7. Risks / honest unknowns
- **TEV→shader (R5) is genuinely hard** — it is the core of GC rendering. Budget accordingly; use
  Dolphin's `PixelShaderGen` only as a *reference for the math*, re-derived natively.
- **Vulkan device ownership vs Dolphin during bring-up** — running our Vulkan alongside Dolphin's on
  one surface is awkward; may need to take the surface fully (R3) earlier than ideal, or render
  offscreen and blit. Decide at R3.
- **Display-list recursion + state persistence** — CP/VAT/array state persists across DLs and frames;
  the decoder must thread it exactly (the existing analyzer already relies on this).
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
