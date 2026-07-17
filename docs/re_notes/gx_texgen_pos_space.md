# GX texgen input space: does GX_TG_POS see object space or eye space?

Underpins the **marukage (projective ground-shadow) flicker** on the 60fps
interpolated in-between field. The interpolator re-issues each model's draw with the
**POS (modelview) matrix BLENDED to the interpolated camera** (eye space). Opaque
geometry interpolates cleanly; the projective shadow — drawn with
`GXSetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_POS, texmtx)` — flickers. The
question: does a `GX_TG_POS`-sourced texgen's output depend on the POS/modelview
matrix?

Sources cited are in-repo: Dolphin's XF/texgen emulation under
`externals/dolphin/Source/Core/VideoCommon/` and the SMS GX library under
`decomp/sms/src/dolphin/gx/`. Read, not guessed.

---

## 1. What vector does the texmtx multiply for GX_TG_POS / NRM / BINRM?

### GC hardware side — `GXSetTexCoordGen2` (`decomp/sms/src/dolphin/gx/GXAttr.c:520`)

The source param is encoded into two XF fields, `row` (the texgen **source row**) and
`form` (the **input form**):

```c
case GX_TG_POS:    row = 0; form = 1; break;   // line 533
case GX_TG_NRM:    row = 1; form = 1; break;   // line 537
case GX_TG_BINRM:  row = 3; form = 1; break;   // line 541
case GX_TG_TANGENT:row = 4; form = 1; break;
case GX_TG_TEX0:   row = 5; break;             // ... TEX0..TEX7 = rows 5..12
```

`row` goes into the XF texgen reg (`SET_REG_FIELD(..., reg, 5, 7, row)` at lines 608 /
614 / 627). `form` is the AB11-vs-ABC1 input form (`reg, 1, 2, form`). For
`GX_TG_POS`, **row = 0**.

### XF emulation side — `SourceRow` enum (`externals/dolphin/Source/Core/VideoCommon/XFMemory.h:86`)

```c
enum class SourceRow : u32 {
  Geom = 0,    // Input is abc   <-- this is GX_TG_POS's row 0
  Normal = 1,  // Input is abc
  Colors = 2,
  BinormalT = 3,
  BinormalB = 4,
  Tex0 = 5, ... Tex7 = 12
};
```

The XF reg field is decoded as `xfmem.texMtxInfo[i].sourcerow` (`XFMemory.h:325`,
`BitField<7,5,SourceRow>`). `GX_TG_POS` → row 0 → `SourceRow::Geom`. Exactly matches
the GX encoding above.

### What the shader actually feeds the texmtx — `VertexShaderGen.cpp`

The texgen **input vector** (`coord`) is selected purely from the **raw vertex
attributes**, before any transform (`VertexShaderGen.cpp:640`):

```c
switch (texinfo.sourcerow) {
case SourceRow::Geom:                              // line 642
    coord.xyz = rawpos.xyz;     break;             //   <-- RAW position attribute
case SourceRow::Normal:
    coord.xyz = rawnormal.xyz;  break;
case SourceRow::BinormalT:
    coord.xyz = rawtangent.xyz; break;
case SourceRow::BinormalB:
    coord.xyz = rawbinormal.xyz;break;
default: /* TexN */ coord = vec4(rawtexN.x, rawtexN.y, 1, 1);
}
if (texinfo.inputform == TexInputForm::AB11) coord.z = 1.0;  // form bit, line 677
```

`rawpos`, `rawnormal`, etc. are the **decoded vertex-buffer attributes** — i.e. the
position/normal **as stored in the vertex stream, in object/model space**. They are
*not* run through the position matrix first.

The texmtx multiply then operates on that raw `coord`
(`WriteTexCoordTransforms`, `VertexShaderGen.cpp:161`):

```c
// for a Regular texgen with a shared (non-per-vertex) texmtx, STQ projection:
result = vec3(dot(coord, I_TEXMATRICES[3*i]),
              dot(coord, I_TEXMATRICES[3*i+1]),
              dot(coord, I_TEXMATRICES[3*i+2]));   // lines 195-197
```

`coord` here is the same raw vector chosen above; `I_TEXMATRICES` is the GX texmtx
(the `mtx` arg of `GXSetTexCoordGen`). **No POS/modelview matrix appears anywhere in
this path.**

### Contrast — the POSITION output is a *separate* transform

The vertex's clip/eye position is computed independently in the same shader body
(`WriteVertexBody`, `VertexShaderGen.cpp:888`):

```c
vertex_output.position = vec4(vertex_input.position * dolphin_position_matrix(), 1.0);
```

`dolphin_position_matrix()` (`VertexShaderGen.cpp:102`) is the **POS/modelview**
(`I_POSNORMALMATRIX` or the per-vertex `I_TRANSFORMMATRICES[posidx]`). This multiply
feeds `vertex_output.position` only — it is consumed by clip projection and lighting.
The **texgen path never reads `vertex_output.position`** for a `GX_TG_POS` Regular
texgen; it reads `vertex_input.texture_coord_i`, which was filled from `rawpos`
(`VertexShaderGen.cpp:687`, `:950`).

> Note: the texgen path *does* read view-space position only for **EmbossMap**
> texgens (`VertexShaderGen.cpp:928`, uses `vertex_output.position`), which is a
> different texgen type and not what the marukage uses.

**Answer to (1):** a `GX_TG_POS` texgen multiplies the texmtx by the **raw vertex
position attribute in object/model space (case (a))** — *before* the POS/modelview
matrix. Same for `GX_TG_NRM` (raw normal) and `GX_TG_BINRM` (raw binormal/tangent).
This is GC hardware behavior (source row 0/1/3 select XF *input* rows, not
post-transform results) and Dolphin reproduces it faithfully.

---

## 2. Does an interpolated POS matrix change the generated texcoord?

A `GX_TG_POS` texgen's output is `texmtx · rawpos`. Neither operand is the POS matrix:

- `rawpos` is the per-vertex object-space attribute — identical across both draws of
  the same model (we re-issue the *same* vertices).
- `texmtx` (`I_TEXMATRICES[...]`, the `mtx` arg) is set by `GXSetTexCoordGen` /
  `GXLoadTexMtxImm` — a constant we hold fixed across the in-between field.

Therefore: **drawing the model with a different (interpolated-camera) POS matrix but
the same texmtx produces the IDENTICAL generated texcoord.** A `GX_TG_POS` projective
shadow is **not** camera-dependent *through the texgen*. Changing only the modelview
matrix does not move the projected texcoord at all.

The only thing the interpolated POS matrix changes is `vertex_output.position` — i.e.
*where on screen* the shadow geometry lands. So the shadow's screen footprint
interpolates, but the texture coordinate sampled at each footprint vertex is computed
from a frame-constant (texmtx · object-pos), which is correct/stable.

---

## 3. GXSetTexCoordGen2 input form, dual-texmtx, and the post-transform (dttmtx)

- **Input form (`form`)** — `form=1` (ABC1) for POS/NRM/BINRM/TANGENT; for TEXn it is
  AB11 (`form` defaults 0). In Dolphin: `if (inputform == AB11) coord.z = 1.0`
  (`VertexShaderGen.cpp:677`). ABC1 keeps the third component (z of rawpos);
  `coord.w` is always 1 (`coord = vec4(...,1.0,1.0)` at `:639`). For a `GX_TG_MTX2x4`
  POS texgen this means the texmtx (a 2×4) sees `(x, y, z, 1)` of the object-space
  position. Still object space — the form bit only chooses whether the input's third
  row is the real z or a forced 1.0; it does not introduce the modelview matrix.

- **`mtx` (the texmtx id)** — selects `I_TEXMATRICES` row (`GXAttr.c:649` writes the
  matIdx; shader indexes `I_TEXMATRICES[3*i+k]`). Constant per draw; not the POS
  matrix.

- **`pt_texmtx` + `normalize` (the dual/post-transform, "dttmtx")** —
  `GXSetTexCoordGen2`'s last two args. Encoded at `GXAttr.c:646-648`
  (`pt_texmtx-64` into the post-matrix index, `normalize` bit). In Dolphin this is the
  **dual-texture transform** (`xfmem.dualTexTrans`, `VertexShaderGen.cpp:207`):

  ```c
  if (dualTexTrans_enabled) {
      P0..P2 = I_POSTTRANSFORMMATRICES[postInfo.index ...];   // line 211
      if (postInfo.normalize) result = normalize(result);     // line 216
      result = vec3(dot(P0.xyz,result)+P0.w, ...);            // post-transform
  }
  ```

  This is a **second constant matrix** applied *after* the texmtx, still with no
  modelview dependence. `GXSetTexCoordGen` (the 1-arg form SMS uses for the shadow)
  sets `pt_texmtx = GX_PTIDENTITY` / `normalize = GX_FALSE`, i.e. identity post-stage —
  irrelevant here. Even if enabled, it adds no camera dependence.

- **The q==0 special case** (`VertexShaderGen.cpp:228`) is a GC quirk on the texgen
  *result* (clamp when result.z==0), independent of the modelview matrix.

So none of the input-form / dual-texmtx / dttmtx knobs introduce a POS-matrix
dependency.

---

## 4. CONCLUSION

**A `GX_TG_POS` (position-sourced) texgen is NOT affected by the POS/modelview
matrix.** It transforms the **raw object/model-space vertex position** by the texmtx
(and optional constant post-transform matrix). The modelview matrix only affects the
*on-screen position* of the geometry (`vertex_output.position`), never the texgen's
generated texcoord. Verified on both sides: GC encoding (`GX_TG_POS` → XF source row
0 = `SourceRow::Geom`, `GXAttr.c:533`) and Dolphin's faithful emulation
(`SourceRow::Geom` → `coord.xyz = rawpos.xyz` → `texmtx·coord`,
`VertexShaderGen.cpp:642`, `:195`), with the position transform a separate multiply
(`:888`).

### Implication for the 60fps interpolation / marukage flicker

Because the texgen reads **object space**, our interpolation of the POS matrix is
**not** what changes the shadow's texcoords — so a `GX_TG_POS` projective shadow is
already consistent at the texgen level across the real and in-between fields **as long
as the shadow's `texmtx` is the same on both**.

The flicker therefore is **not** explained by GX_TG_POS being eye-space-dependent (it
isn't). Two real candidate causes remain, both of which this analysis re-focuses the
search onto:

1. **The shadow's projection texmtx is itself camera/view-derived and is NOT being
   held constant (or not being re-blended consistently) on the in-between field.**
   SMS's marukage projector builds `texmtx` from a light/eye-relative projection. If
   the real field uses `texmtx_view(cam_real)` while the geometry is re-issued with
   the interpolated POS matrix `POS(cam_mid)`, the shadow's *texcoords* still come
   from `texmtx_view(cam_real)` but the *geometry* lands at the midpoint position —
   a mismatch (texcoords computed for one camera, footprint drawn for another). The
   fix is consistency: **on the in-between field, the shadow's projection texmtx must
   be rebuilt with the SAME (interpolated) view used for the POS matrix**, OR the
   POS-matrix space for that draw must be **world** rather than eye so the
   object-space input the texmtx sees is unaffected by camera blending. Whichever, the
   texmtx and the POS matrix must reference the **same camera** on a given field.

2. **The shadow geometry isn't actually being interpolated the same way as the opaque
   model** (e.g. it's emitted in a separate pass with a different/non-blended matrix,
   or its texmtx upload is skipped on the in-between re-issue), so it snaps between the
   real-field state and a stale/zeroed state = visible blink.

Net: stop suspecting GX_TG_POS of eye-space behavior. The texgen is object-space and
frame-stable for fixed inputs; **the flicker is a texmtx/POS-matrix *consistency*
problem on the in-between field, not a property of GX_TG_POS.** To keep the projective
shadow consistent, ensure the shadow's projection texmtx and the model's POS matrix
are derived from the *same* (interpolated) camera for each field — or build the POS
matrix in world space so the object-space texgen input never depends on the camera at
all.
