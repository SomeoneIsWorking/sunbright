# Decoding a J3DShape into triangles from guest memory (US/GMSE01)

RE spec for the PC-native renderer, which overrides `J3DShape::draw` (0x802e0390) and reads the
game's structures out of GUEST memory — all big-endian, 32-bit pointers, no C++ casts available.

**Vtable rule.** `J3DShape`, `J3DVertexData`, `J3DMaterial`, `J3DDrawMtxData` have NO vtable.
`J3DShapeDraw`, `J3DShapeMtx`(+subclasses), `J3DTexture`, `J3DTevBlock`(+subclasses), `J3DPacket`
(+subclasses) DO — vptr at +0x00, so header offsets starting at 0x0 are shifted by 4.

## 1. J3DVertexData (+0x44 on the shape) — no vtable, size 0x44
`0x00 vtxCount · 0x04 nrmCount · 0x08 clr0Count · 0x0C mVtxAttrFmtList · 0x10 pos · 0x14 nrm ·
0x18 nbt · 0x1C clr0 · 0x20 clr1 · 0x24+4i tex0..7`

**⚠️ Use the LIVE array bases, not these**, for POS/NRM/CLR0 — `loadVtxArray` binds them from
`j3dSys` (swapped per frame for skinning/colour anim):
- **j3dSys base (US) = 0x804045DC**
- POS `[j3dSys+0x10C]` · NRM `[j3dSys+0x110]` (only when `shape.unk30 == 0`) · CLR0 `[j3dSys+0x114]`
- TEX0..7 stay `mVertexData->mVtxTexCoordArray[i]`

`GXVtxAttrFmtList` entries are **16 bytes**: `0x00 attr · 0x04 cnt · 0x08 type · 0x0C u8 frac`.
Terminate on `attr == 0xFF`.

**J3D's baked strides (use these, they are what the hardware was told):** POS/NRM = 12 if F32 else 6;
CLR0/CLR1 = 4; TEXn = 8 if F32 else 4. NBT: if a desc entry is `GX_VA_NBT`, `shape.unk30=1`, NRM
stride ×3 and base becomes `mVtxNBTArray`.

Element read: `base + i*stride`; F32 as-is; U8/S8/U16/S16 → BE int (sign-extend) `/ (1<<frac)`.
**⚠️ Normals: hardware ignores the VAT frac and uses a fixed shift (S8 → 2^6, S16 → 2^14). SMS BMDs
set frac=14 so they agree — but if an S16 normal ever has frac==0, use 14.**

## 2. Vertex descriptor — `mVtxDescList` (+0x2C)
`GXVtxDescList` entries are **8 bytes**: `0x00 attr · 0x04 type`. Terminate on `attr == 0xFF`.
`GXAttrType`: NONE=0, DIRECT=1, INDEX8=2, INDEX16=3.
`GXAttr`: PNMTXIDX=0, TEX0-7MTXIDX=1..8, POS=9, NRM=10, CLR0=11, CLR1=12, TEX0..7=13..20, NBT=25.
NBT maps onto the NRM slot with `cnt = GX_NRM_NBT`.

**J3D geometry always uses GX_VTXFMT0**, so the low 3 bits of every draw opcode are 0 and the format
table can be built once per shape.

**Vertex byte size** — attributes in GXAttr enum order, offsets accumulating in the same order:
NONE→0; DIRECT→`compSize*compCount`; INDEX8→1 (3 if NRM is NBT3); INDEX16→2 (6 if NBT3).

## 3. Geometry display list — per element `[shape+0x38][i]`
`J3DShapeDraw`: `0x00 vptr · 0x04 u32 size · 0x08 const u8* displayList`.

Parse: `cmd = dl[pos++]`; `0x00` = NOP (padding); `cmd & 0xF8` in {0x80 QUADS, 0x90 TRIANGLES,
0x98 STRIP, 0xA0 FAN, 0xA8 LINES, 0xB0 LINESTRIP, 0xB8 POINTS} → `vtxCount = BE u16`, then
`vtxCount * vtxSize` bytes of payload. **Anything else is a desync — fail fast, do not continue.**

Triangulation (v = 0-based ordinal): TRIANGLES `(v,v+1,v+2)` step 3 · STRIP emit (0,1,2) then even v
`(v-2,v-1,v)`, odd v `(v-1,v-2,v)` · FAN emit (0,1,2) then `(0,v-1,v)` · QUADS step 4 →
`(v,v+1,v+2)` and `(v+2,v+3,v)`. Keep GC winding; set cull from the material's PE block.

## 4. Transform
`drawMtxArray = [ [shape+0x50] + 4 * *[shape+0x58] ]`, entries `Mtx = f32[3][4]` (48 B).
Normals `[shape+0x54]`, `Mtx33 = f32[3][3]` (36 B).
Bounds: `[shape+0x48]` = `J3DDrawMtxData` — `u16 mEntryNum @0x00` is the valid range FOR THIS SHAPE
(using the model's instead is the file-select-Mario mangling bug).

Per element, `mtxObj = [shape+0x34][i]` (`J3DShapeMtx*`, vptr@0x00):
- base `J3DShapeMtx`: `u16 unk4 @0x04`, all verts use `drawMtxArray[unk4]`
- `J3DShapeMtxMulti`: `u16 count @0x08`, `u16* table @0x0C`; **`0xFFFF` = slot not loaded, skip**
- Discriminate by calling the guest virtuals: `getUseMtxNum` at `vptr+0x10`, `getUseMtxIndex` at
  `vptr+0x14` (CW vtable = 8-byte header then fn ptrs in declaration order)

`PNMTXIDX` byte is the GX matrix ADDRESS: `slot = byte/3`, `guestIdx = getUseMtxIndex(slot)`.

**⚠️ Pipeline selector — do not skip.** `pipeline = (shape.unk8 >> 2) & 3` changes which matrix loads:
0 PNGP = draw/nrm arrays · 1 PCPU = **position from j3dSys.mViewMtx** · 2 NCPU = normal from viewMtx ·
3 PNCPU = both from viewMtx. Set for CPU-skinned meshes; getting it wrong double-transforms Mario.

**Draw matrices are ALREADY model×view** (viewCalc concats the view matrix), so:
`p_view = M * (x,y,z,1)` with `M[r][c]` at `mtxBase + (r*4+c)*4`, then `p_clip = P * vec4(p_view,1)`.
`P` comes from the game's own `GXSetProjection` (0x80362c34, already hooked in widescreen.cpp) —
cache the full 4x4 there; the 6-element GX vector discards terms. GC clip z ∈ [-w, 0].

## 5. Material / texture
No back-pointer on the shape — it is ambient state set by the enclosing `J3DMatPacket::draw`:
`matPacket = [j3dSys+0x3C]` · `texture = [j3dSys+0x54]` · `material = [matPacket+0x38]`.
`J3DMaterial` (no vtable): `mShape@0x04` (cross-check), `mTevBlock@0x28`, `mPEBlock@0x30`.
`texNo[map] = BE u16 at [material+0x28] + 0x04 + 2*map`; **0xFFFF = no texture**.

`J3DTexture` HAS a vptr: `0x00 vptr · 0x04 u16 count · 0x08 ResTIMG* resources`.
**⚠️ INFERRED, not read — validate on first use**; if `[tex+0x00]` is not code/data-ish, fall back to
count@0x00 / ptr@0x04.

`ResTIMG` (size 0x20): `0x00 format (& 0xF) · 0x02 w · 0x04 h · 0x06 wrapS · 0x07 wrapT ·
0x08 isIndex · 0x09 palFmt · 0x0A numColors · 0x0C palOffset · 0x14 minFilter · 0x18 mipCount ·
0x1C imageDataOffset`. **⚠️ imageDataOffset and paletteOffset are SIGNED and relative to the ResTIMG
itself** — a naive unsigned add produces a wild pointer.
GXTexFmt: I4=0 I8=1 IA4=2 IA8=3 RGB565=4 RGB5A3=5 RGBA8=6 CMPR=0xE; CI: C4=8 C8=9 C14X2=0xA.

**Open gap (flagged, not guessed):** `J3DTexGenBlock` subclass offsets were not derived — feed TEX0
straight through for a first pass.

## Implementation order
1. Hook 0x802e0390; read `shape+0x50/0x58` → drawMtxArray; bail if null (model not update()d yet).
2. Build `desc[21]` (8B entries) + `fmt[21]` (16B entries), NBT fixup, compute vtxSize + offsets.
3. Bind live array bases and strides.
4. Read the pipeline selector.
5. Per element: parse the DL, resolve PNMTXIDX → slot → guestIdx → matrix, transform, triangulate.
6. Material/texture from j3dSys.
