# J3D model draw / transform pipeline (RE for 60 fps interpolation)

Reverse-engineered from `decomp/sms/` (doldecomp/sms). Addresses from
`reference/sms_gmse01_funcs.txt`. This documents exactly what produces a model's
on-screen transform, what RAM buffer the GPU reads, and the per-frame state an
in-between re-issue must respect. It backs the substrate in
`runtime/overrides/interp_capture.cpp` (which is already correct — see the
CONCLUSION cross-check at the end).

All struct offsets below are the decomp's `J3DModel` / `J3DShapePacket` /
`J3DDrawMtxData` layouts, which match the live SMS binary (verified by the
interp code reading `model+0x7C`, `model+0x60/0x64`, `data+0x98`, etc.).

---

## 0. The objects and their offsets

`J3DModel` (size 0xA0) — `include/JSystem/J3D/J3DGraphAnimator/J3DModel.hpp:199`:

| off  | field                | meaning |
|------|----------------------|---------|
| 0x00 | vtable               | `viewCalc` is the 4th virtual (`update`/`entry`/`calc`/`viewCalc`/`~`) |
| 0x04 | `mModelData`         | `J3DModelData*` (shared, read-only geometry/skeleton) |
| 0x08 | `unk8`               | model flags (bit0 = "matrices are pre-baked / no view concat", bit2/3 deform) |
| 0x50 | `mScaleFlagArr`      | per-joint scale flags |
| 0x58 | `mNodeMatrices`      | `Mtx*` — per-JOINT world matrices (output of `calc`/`update`, model space) |
| 0x5C | `unk5C`              | `Mtx*` — weighted-envelope matrices (skinning), world space |
| 0x60 | `mDrawMtxBuf[0]`     | `Mtx**` — draw-matrix double buffer, bank 0 |
| 0x64 | `mDrawMtxBuf[1]`     | `Mtx**` — draw-matrix double buffer, bank 1 |
| 0x68 | `mNrmMtxBuf[0]`      | `Mtx33**` — normal-matrix double buffer, bank 0 |
| 0x6C | `mNrmMtxBuf[1]`      | `Mtx33**` — normal-matrix double buffer, bank 1 |
| 0x70 | `mBumpMtxArr[0..1]`  | NBT/bump matrix double buffer (only models with NBT scale) |
| 0x7C | `mCurrentViewNo`     | `u32` — the **view index** (see §1.2) |
| 0x80 | `mMatPackets`        | `J3DMatPacket[mMaterialNum]` |
| 0x84 | `mShapePackets`      | `J3DShapePacket[mShapeNum]` |

`J3DDrawMtxData` (in `J3DModelData` at offset 0x98) —
`include/JSystem/J3D/J3DGraphBase/J3DVertex.hpp:118`:

| off  | field                | meaning |
|------|----------------------|---------|
| 0x00 | `mEntryNum`          | **draw-matrix count** = entries in each `mDrawMtxBuf[b][v]` array |
| 0x02 | `mDrawFullWgtMtxNum` | count of "full weight" (single-joint, non-skinned) draw matrices |
| 0x04 | `mDrawMtxFlag`       | per-entry: 0 = single-joint, !=0 = weighted/envelope |
| 0x08 | `mDrawMtxIndex`      | per-entry: joint index (flag==0) or envelope index (flag!=0) |

So in RAM: `mEntryNum = mem_r16(mModelData + 0x98)`, and each draw-matrix bank
`mDrawMtxBuf[b][v]` is a contiguous array of `mEntryNum` `Mtx` (each `Mtx` =
3×4 f32 = 48 bytes), 32-byte aligned (`new (0x20)` at
`J3DModel.cpp:532`).

---

## 1. `J3DModel::viewCalc` — 0x802deeb8 (`viewCalc__8J3DModelFv`)

Source: `J3DModel.cpp:780-813`. This is the per-frame function that turns the
already-computed per-joint world matrices into **draw matrices** (view-space,
ready for the GPU) and writes them into the draw-matrix buffer.

### 1.1 What it computes (in order)

```
swapDrawMtx();   // J3DModel.hpp:252 — swap mDrawMtxBuf[0][v] <-> mDrawMtxBuf[1][v]
swapNrmMtx();    // J3DModel.hpp:259 — swap mNrmMtxBuf[0][v] <-> mNrmMtxBuf[1][v]

if (checkFlag(1)) {                       // bit0 set: matrices are PRE-BAKED
    // copy mNodeMatrices[drawMtxIndex[i]] straight into draw matrices (no view mul)
    for i in DrawFullWgtMtxNum: MTXCopy(getAnmMtx(drawMtxIndex[i]), getDrawMtx(i));
    for i in WEvlpMtxNum:       MTXCopy(unk5C[i], getDrawMtx(DrawFullWgtMtxNum + i));
} else {                                  // normal path
    viewMtx = j3dSys.getViewMtx();        // camera view matrix (this frame's eye)
    // single-joint draw mtx = viewMtx * mNodeMatrices[drawMtxIndex[i]]
    J3DMTXConcatArrayIndexedSrc(viewMtx, mNodeMatrices, mDrawMtxData.mDrawMtxIndex,
                                getDrawMtxPtr(), DrawFullWgtMtxNum);   // 0x802d32cc
    // weighted/envelope draw mtx = viewMtx * unk5C[k]  (appended after the singles)
    J3DPSMtxArrayConcat(viewMtx, getWeightAnmMtx(0),
                        getDrawMtx(DrawFullWgtMtxNum), WEvlpMtxNum);   // 0x802d3404
}

calcNrmMtx();    // 0x802df0f0 — inverse-transpose of each draw mtx -> mNrmMtxBuf[1][v]
calcBBoard();    // billboard fix-up of draw matrices (rip rotation, keep scale)
calcBumpMtx();   // NBT-scale matrices, only if mModelData->unk18 == 1
DCStoreRange(getDrawMtxPtr(), mEntryNum * sizeof(Mtx));   // flush to RAM for the GPU
DCStoreRange(getNrmMtxPtr(),  mEntryNum * sizeof(Mtx33));
prepareShapePackets();   // 0x802df844 — wire the live buffer into the shape packets
```

Key accessors (`J3DModel.hpp:243-257`):

```
getDrawMtxPtr()  = mDrawMtxBuf[1][mCurrentViewNo]   // the "current" draw mtx array
getDrawMtx(i)    = mDrawMtxBuf[1][mCurrentViewNo][i]
getNrmMtxPtr()   = mNrmMtxBuf[1][mCurrentViewNo]
```

So **`viewCalc` writes the new frame's draw matrices into bank `[1]`** (after the
swap). The crucial consequence for interpolation: because `swapDrawMtx()` runs
FIRST, immediately after one real field's `viewCalc`:

- `mDrawMtxBuf[0][view]` holds **tick N-1** (the previous field's result — the
  pointer that was `[1]` last frame is now `[0]`),
- `mDrawMtxBuf[1][view]` holds **tick N** (just computed, the one shapes will load).

This is the double-buffer the interpolator blends. (`interp_capture.cpp:67-70`
reads exactly these: `d0a=model+0x60` → tick N-1, `d1a=model+0x64` → tick N.)

### 1.2 The double-buffer `mDrawMtxBuf[2][...]` and the "view" index

Two independent indices:

- **Bank index `[0]`/`[1]`** — the temporal double buffer. Allocated as two
  parallel `Mtx*[viewNum]` arrays in `entryModelData` (0x802ddf90,
  `J3DModel.cpp:521-536`); each `[b][v]` element is its own
  `Mtx[mEntryNum]` block. `swapDrawMtx()`/`swapNrmMtx()` swap **only the pointer**
  for the current view (`mDrawMtxBuf[b][mCurrentViewNo]`), so the previous
  frame's matrices survive in the other bank — they are not recomputed, just
  re-pointed. This is what makes N-1 available "for free."

- **View index `mCurrentViewNo` (model+0x7C)** — selects WHICH set of draw
  matrices within a bank. The 3rd ctor arg (`mtx_buffer_flag` / `param_3` in
  `entryModelData`) is the **view count**: `mDrawMtxBuf[i] = new Mtx*[param_3]`
  (`J3DModel.cpp:523`). For almost all SMS models the view count is 1 and
  `mCurrentViewNo` stays 0; it exists so one model can be drawn from multiple
  cameras/eyes in the same frame (split-screen / reflection passes) with each
  view keeping its own draw-matrix array and its own N/N-1 history.
  `mCurrentViewNo` is set by the draw machinery before `viewCalc` (the model's
  `viewCalc` runs once per active view). For interpolation purposes, read it
  from `model+0x7C` and index with it; do not assume 0 blindly (the interp code
  guards `view > 16`, `interp_capture.cpp:65`).

`swapNrmMtx` mirrors `swapDrawMtx` for the normal matrices; both are swapped
every `viewCalc` so the normal buffer's N-1/N alignment matches the draw buffer.

---

## 2. Path from draw matrices to the GPU (indexed matrix loads)

### 2.1 `prepareShapePackets` wires the LIVE bank into each shape packet

`J3DModel.cpp:962-998`. For every shape `i`:

```
pkt = &mShapePackets[i];
pkt->setDrawMtx(mDrawMtxBuf[1]);     // pkt->unk18 = Mtx**  (bank [1], all views)
pkt->setNrmMtx (mNrmMtxBuf[1]);      // pkt->unk1C = Mtx33**
pkt->setCurrentViewNoPtr(&mCurrentViewNo);   // pkt->unk20 = &model->mCurrentViewNo
pkt->unk24/28/2C = vertex array base pointers (pos/nrm/color)
```

`J3DShapePacket` (set at `J3DPacket.cpp:131-141`):
`unk14`=shape, `unk18`=`mDrawMtxBuf[1]`, `unk1C`=`mNrmMtxBuf[1]`,
`unk20`=`&mCurrentViewNo`. **Note packets always point at bank `[1]`** — they
never see bank `[0]`. (`interp_capture.cpp:149` reads `pkt->unk18` and confirms
`unk18[view] == cur`, the N buffer.)

### 2.2 `J3DShapePacket::draw` → `J3DShape::draw`

`J3DShapePacket::draw` (`J3DPacket.cpp:145-164`):

```
j3dSys.unk10C/110/114 = pkt->unk24/28/2C;       // vertex array bases
shape->mDrawMatrices  = pkt->unk18;             // = mDrawMtxBuf[1] (Mtx**)
shape->mNormMatrices  = pkt->unk1C;             // = mNrmMtxBuf[1]
shape->mCurrentViewNo = pkt->unk20;             // = &model->mCurrentViewNo
shape->draw();
```

`J3DShape::draw` (`J3DShape.cpp:223-242`) — THE GPU hand-off:

```
GXCallDisplayList(mGDCommands, 0xC0);           // VCD/VAT setup (vtx format)
J3DShapeMtx::currentPipeline = (unk8 >> 2) & 3; // which load pipeline
loadVtxArray();                                 // GXSetArray for POS/NRM/CLR vtx data

j3dSys.setModelDrawMtx(mDrawMatrices[*mCurrentViewNo]);  // <-- pos-mtx ARRAY BASE
j3dSys.setModelNrmMtx (mNormMatrices[*mCurrentViewNo]);  // <-- nrm-mtx ARRAY BASE

for i in mElementCount:
    mMatrices[i]->load();    // emit GXLoadPosMtxIndx / GXLoadNrmMtxIndx (indexed)
    mDraws[i]->draw();       // GXCallDisplayList of the primitive vertices
```

`mDrawMatrices[*mCurrentViewNo]` = `mDrawMtxBuf[1][mCurrentViewNo]` = the **N
draw-matrix array in RAM** (`getDrawMtxPtr()` from §1). This is the exact buffer
the interpolator blends.

### 2.3 `setModelDrawMtx` sets the CP indexed-array base (array 12)

`J3DSys::setModelDrawMtx` (`J3DSys.hpp:45-49`):

```
GXSetArray(GX_POS_MTX_ARRAY, mtx, sizeof(Mtx));    // base = mDrawMtxBuf[1][view]
```

`GXSetArray` (`GXAttr.c:502-516`): `cpAttr = attr - GX_VA_POS`. With
`GX_VA_POS=9` and `GX_POS_MTX_ARRAY=21` (`GXEnum.h:94,106`), `cpAttr = 12` — this
is the **CP "array 12" = the XF position-matrix indexed array**. It writes two CP
registers: base address (`cpAttr|0xA0`, physical = `base & 0x3FFFFFFF`) and
stride (`cpAttr|0xB0`, = `sizeof(Mtx)` = 48). `setModelNrmMtx` does the same for
`GX_NRM_MTX_ARRAY` → `cpAttr = 13` (the normal-matrix array, stride
`sizeof(Mtx33)` = 36).

### 2.4 The indexed matrix load reads from that base

`J3DShapeMtx::load` (`J3DShape.cpp:41-45`) selects a pipeline
(`mtxLoadPipeline[currentPipeline]`, `J3DShape.cpp:9-14`):

- `loadMtxIndx_PNGP` (default, GP path) → `j3dSys.loadPosMtxIndx(id, mtx_index)`
  and `loadNrmMtxIndx(id, mtx_index)`.

`J3DSys::loadPosMtxIndx` (`J3DSys.cpp:53-56`) → `GXLoadPosMtxIndx(mtx_index, id*3)`.

`GXLoadPosMtxIndx` (`GXTransform.c:166-179`) emits CP opcode **0x20** with the
matrix index and the XF destination row (`id*3`). The hardware then DMA-reads
**48 bytes from `array12_base + mtx_index * stride`** into XF pos-matrix memory.
`GXLoadNrmMtxIndx3x3` (`GXTransform.c:196-209`) emits opcode **0x28** for the
normal array (array 13).

So the GPU's indexed matrix load reads from:

```
GPU pos matrix[id] = mDrawMtxBuf[1][mCurrentViewNo][mtx_index]
                     ^-- array12 base set by setModelDrawMtx
```

**This is the load-bearing fact for interpolation:** the bytes the GPU consumes
for a draw are precisely `mDrawMtxBuf[1][mCurrentViewNo]` (the N buffer). Editing
those `mEntryNum` `Mtx` in place before the shape draws → the GPU's indexed load
sees the edited (blended) matrices. No separate "uploaded copy" exists; the CP
array base points straight at the model's heap buffer. (`viewCalc` already
`DCStoreRange`'d it, so it is coherent in main RAM; an in-between re-issue that
edits it on the host CPU must respect the same coherency — our memory bridge does
host load+bswap directly into that RAM, so the GPU frontend re-reads it.)

`mtx_index` per shape: single-matrix shapes use `J3DShapeMtx` with one fixed
index (`unk4`, `J3DShape.hpp:33`); multi-matrix shapes use `J3DShapeMtxMulti`
(§4).

---

## 3. J3DModel vs the SMS world/map model (SDLModel) — DIFFERENT buffering

SDLModel (`include/M3DUtil/SDLModel.hpp`, `src/M3DUtil/SDLModel.cpp`) **derives
from J3DModel** (shares the same 0x60/0x64 draw buffers and 0x7C view index) and
is used for static/instanced map geometry. It allocates its draw buffers the same
way (`entryModelDataSDL`, `SDLModel.cpp:139-154`), but its per-frame transform
function is **different**:

`SDLModel::viewCalcSimple` — 0x8023d36c (`SDLModel.cpp:294-301`):

```
swapDrawMtx();                                  // swaps mDrawMtxBuf only
mA = gpCamera->getUnk1EC();                     // a precomputed view matrix
for i in mEntryNum:
    MTXConcat(mA, mNodeMatrices[i], getDrawMtx(i));   // draw = view * node, DIRECT
DCStoreRange(getDrawMtxPtr(), mEntryNum * sizeof(Mtx));
```

Differences from `J3DModel::viewCalc`, all relevant to interpolation:

1. **No normal-matrix double-buffer maintenance.** It calls `swapDrawMtx()` only,
   NOT `swapNrmMtx()`, and never runs `calcNrmMtx`. SDL map geometry doesn't
   re-derive normal matrices per frame here.
2. **No envelope/weighted path, no billboard, no bump.** It is a flat
   `view × node` for every entry, indexed directly by `i` (not via
   `mDrawMtxIndex`). `mNodeMatrices[i]` is used 1:1 with draw entry `i`.
3. **No `prepareShapePackets`** in this function — SDL packet wiring is done in
   the SDL entry/`entrySameMat` path (`SDLModel.cpp:11-99`), which shares one
   matrix packet across instanced copies of the same material.
4. Still double-buffers the **draw** matrices identically: after a real field,
   `mDrawMtxBuf[0][view]` = N-1, `mDrawMtxBuf[1][view]` = N, and shapes load
   `[1][view]` via the same `setModelDrawMtx` → array-12 path.

So: **both double-buffer the draw matrices the same way** (so the §5 blend works
for both), **but SDLModel does NOT double-buffer/recompute normal matrices** and
does not use the indexed-source/envelope concat. The interp code tracks SDL
models in a separate set (`g_sdl_set`, registered via the
`SDLModel::viewCalcSimple` tee — `interp_capture.cpp:44,99`) precisely because
the buffer semantics differ; the draw-matrix blend is valid, but do not assume an
N-1 normal-matrix buffer exists for SDL.

> Uncertainty: `MapModel` (`src/Map/MapModel.cpp`) and `MActor`
> (`src/M3DUtil/MActor.cpp`, `viewCalc__6MActor` 0x80239734) are thin wrappers
> that ultimately drive a `J3DModel`/`SDLModel`; they were not deep-read here.
> They funnel into one of the two `viewCalc` variants above, so the buffer model
> is one of the two documented cases — but if a specific actor's matrices look
> wrong, confirm which `viewCalc` its model class resolves to.

---

## 4. Skinning / envelope matrices and "unused slot" garbage

### 4.1 Single-matrix vs multi-matrix shapes

A `J3DShape` carries an array of `J3DShapeMtx*` (`mMatrices`, `J3DShape.hpp:156`),
one per "matrix group" (`mElementCount`). Three concrete types
(`J3DShape.hpp:8-67`):

- **`J3DShapeMtx`** (single): one `mtx_index` (`unk4`). `load()` emits one
  indexed pos+nrm load (`J3DShape.cpp:17-21`). Rigid, single-joint shapes.
- **`J3DShapeMtxMulti`** (skinned/envelope): a table `unkC[unk8]` of `mtx_index`
  values. `load()` (`J3DShape.cpp:59-67`) loops over `unk8` slots and emits an
  indexed load **for each slot whose index != 0xFFFF**:
  ```
  for i in unk8:
      if (unkC[i] == 0xffff) continue;   // <-- SKIPPED slot
      load pos/nrm mtx id=i from array12[ unkC[i] ]
  ```
- **`J3DShapeMtxDL`**: pre-baked matrix display list (`load()` just calls the DL).

The draw-matrix entries that feed `J3DShapeMtxMulti` are produced by the
**weighted/envelope half of `viewCalc`** (`J3DPSMtxArrayConcat`, §1.1): these are
`view × unk5C[k]` (envelope matrices), stored in draw entries
`[DrawFullWgtMtxNum .. mEntryNum)`. The single (full-weight) draw entries
`[0 .. DrawFullWgtMtxNum)` come from `J3DMTXConcatArrayIndexedSrc`.

### 4.2 Where the ~4.8e22 garbage in unused slots comes from

`mDrawMtxBuf[b][v]` is sized `mEntryNum` and allocated with **uninitialized**
`new (0x20) Mtx[mEntryNum]` (`J3DModel.cpp:532`). `viewCalc` only writes:

- entries `[0, DrawFullWgtMtxNum)` (single), and
- entries `[DrawFullWgtMtxNum, DrawFullWgtMtxNum + WEvlpMtxNum)` (envelope).

If `mEntryNum > DrawFullWgtMtxNum + WEvlpMtxNum` (it can be — the buffer is sized
for the max needed across views/shapes), the **tail entries are never written**
and retain heap garbage (often denormal/huge floats — the observed move-magnitude
~4.8e22). They are harmless on hardware because:

- the shapes that reference them use `J3DShapeMtxMulti` with `unkC[i] == 0xffff`
  for those slots, so `load()` **skips** them (`J3DShape.cpp:63`) — the GPU never
  loads a matrix from a garbage index, and
- no primitive's vertices reference a `mtx_index` into an unwritten slot.

For the interpolator this means: **a large per-entry delta between N-1 and N in
some entries is NOT a real teleport — it is an unwritten/garbage slot in a
skinned model's buffer.** Blending garbage is still harmless (it goes to a slot
nothing loads), but it must NOT be used as a motion/cut heuristic. The interp
code already learned this: the old magnitude/teleport threshold was removed for
exactly this reason (`interp_capture.cpp:122-125`: "misfired on … skinned-model
unused slots"; the memory note records "move_max~4.8e22 was a HARMLESS red
herring (skinned-model unused slots)"). Cut detection moved to the camera level.

> Practical rule: when scanning a draw buffer, you cannot tell a written slot
> from a garbage slot purely from the `Mtx` contents. The authoritative "which
> entries are live" comes from each shape's `J3DShapeMtx`/`...Multi` index lists
> (and `DrawFullWgtMtxNum + WEvlpMtxNum`). For the blend it doesn't matter
> (blend-then-skip is safe); for any *analysis* of motion, restrict to entries
> `< DrawFullWgtMtxNum + WEvlpMtxNum` or NaN/inf-guard each entry.

---

## 5. CONCLUSION — what an in-between re-issue must blend and respect

**What the GPU actually reads.** For each shape drawn, the GPU's indexed
pos-matrix load (CP opcode 0x20, "array 12") reads 48-byte `Mtx` entries from the
CP array base set by `setModelDrawMtx`, which is
`mDrawMtxBuf[1][mCurrentViewNo]` (the model's own heap buffer, made coherent by
`viewCalc`'s `DCStoreRange`). Normals come the same way from
`mNrmMtxBuf[1][mCurrentViewNo]` (array 13). There is no separate uploaded copy —
editing those bytes in RAM before the shape draws changes what the GPU consumes.

**What must be blended to interpolate a model's on-screen transform.** Per model,
per active view `v = mem_r32(model+0x7C)`:

- `prev = mDrawMtxBuf[0][v]` (= `mem_r32(model+0x60) [v]`) — tick N-1
- `cur  = mDrawMtxBuf[1][v]` (= `mem_r32(model+0x64) [v]`) — tick N (GPU reads this)
- `n    = mem_r16(mModelData+0x98)` — `mEntryNum` matrices, each 3×4 f32 (48 B)

Blend `cur[i] = (1-α)·prev[i] + α·cur[i]` for all 12 floats of all `n` entries
(NaN/inf-guard each — §4.2), then issue the draw, then restore `cur` to the saved
tick-N values after present so the guest's double buffer is byte-identical to what
the real field produced (`interp_capture.cpp:103-108, 132-133, 160-168`). The
draw matrices already fold in the view (camera) transform from this field's
`viewCalc`, so blending them interpolates BOTH object motion and camera motion in
eye space — which is why large-but-smooth deltas during camera pans are not cuts
(handle cuts at the camera level, not via a per-matrix magnitude threshold).

For correctness you also want the **normal matrices** blended in lockstep for
`J3DModel` (`mNrmMtxBuf[0/1][v]`, 3×3 f32, 36 B each) so lighting tracks the
geometry; the current substrate blends draw matrices (position) — note that
`SDLModel` does NOT keep an N-1 normal buffer (§3), so a normal blend is only
valid for true `J3DModel` instances.

**Per-frame state an in-between re-issue MUST respect:**

1. **Bank `[1]` is what shapes load, not `[0]`.** Shape packets are wired to
   `mDrawMtxBuf[1]`/`mNrmMtxBuf[1]` by `prepareShapePackets` (`unk18`/`unk1C`).
   Write the blend into bank `[1]` (= `cur`). Do not swap banks for the
   in-between; that would lose N or recompute it.
2. **The view index `mCurrentViewNo` (model+0x7C)** selects the live array within
   the bank and is dereferenced through the packet's `unk20` pointer at draw
   time. Read it; don't assume 0. Blend/restore the array for that view.
3. **Don't call `viewCalc` again** for the in-between — it would `swapDrawMtx()`
   (rotating N-1 into the live bank and computing a fresh frame), destroying the
   N/N-1 pair. Re-issue the *draw pass only* against the edited bank `[1]`.
4. **Coherency:** the buffer was `DCStoreRange`'d by `viewCalc`; an edit on the
   host CPU must be visible to the GX frontend that re-reads array 12 (our memory
   bridge writes straight into that RAM, so this holds).
5. **Garbage tail slots** (entries beyond `DrawFullWgtMtxNum + WEvlpMtxNum`) carry
   heap junk (~1e22) but are skipped by `J3DShapeMtxMulti::load` (index 0xFFFF);
   blend them harmlessly but never treat their delta as motion/cut signal.
6. **SDLModel** (map geometry, `viewCalcSimple` 0x8023d36c) double-buffers draw
   matrices the same way (blend is valid) but has no N-1 normal buffer and uses a
   direct `view × node[i]` layout — track it separately from `J3DModel`.

This matches the implemented substrate in
`runtime/overrides/interp_capture.cpp` (offsets 0x60/0x64/0x7C/0x98, bank-[1]
edit + post-present restore, NaN guard, SDL split set), which is therefore
**correct** against the decomp; the documented uncertainties are the per-actor
`viewCalc` resolution (§3) and whether normal-matrix blending should be added for
lighting fidelity on `J3DModel`.
