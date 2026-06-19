# 2026-06-19 — Indirect texturing (gap #3): RE + reachability + port plan

Continuing `scratch/handoff_2026-06-19_render_gaps.md`. Gap #3 primary = indirect texturing
(`mIndTevStage`). Faithful port; verify by oracle/screenshot, NOT by chasing the wash.

## Reachability gate (tooling-first) — PASSES
Added an IndBlock usage probe to `capture_material` (ngx_j3d_shape.cpp) + a line in `/ngxshape`.
Plaza (`freeroam_plaza.sav`) reports:
```
IndBlock: full(IBLF)=2574 null(IBLN)=127134 none=0 unk=0  max_stages=1
  IndTexStageNum histogram [0..4]: 169 2405 0 0 0
```
⇒ 2405 of 129708 material captures use **exactly 1 indirect stage**. The feature IS exercised in a
reachable scene, and `max_stages=1` keeps the port tractable. (169 IBLF blocks have 0 stages = inert.)

## Data model (verified vs reference/sms)
- **J3DMaterial**: mTevBlock @ 0x28, **mIndBlock @ 0x2C**.
- **J3DIndBlock** variants by getType (vtable slot 2 = vtable+0x10, decoded like pe_block_type):
  'IBLF' = J3DIndBlockFull (has fields), 'IBLN' = J3DIndBlockNull (no indirect).
  J3DIndBlockFull layout: `mIndTexStageNum`@0x04 (u8); `mIndTexOrder[4]`@0x05 (4B each:
  mCoord@0,mMap@1); `mIndTexMtx[3]`@0x18 (0x1C each: `mOffsetMtx[2][3]` f32@0x00, `mScaleExp` s8@0x18);
  `mIndTexCoordScale[4]`@0x6C (mScaleS@0, mScaleT@1).
- **mIndTevStage[]** lives in the **TevBlock** (not IndBlock), variant offsets:
  TVB1@0x12, TVB2@0x59, TVB4@0x7A, TVB16@0x12A. Entry = J3DIndTevStage = 0xC bytes:
  mIndStage@0, mIndFormat@1, mBiasSel@2, mMtxSel@3, mWrapS@4, mWrapT@5, mPrev@6, mLod@7, mAlphaSel@8.

## GX indirect math (from externals/dolphin PixelShaderGen.cpp + GXBump.c — the hardware spec)
Everything is in **fixpoint = normalized_uv * texsize * 128** (texel << 7). GLSL gets texsize from
`textureSize(tex[m],0)`.
1. `fixpoint_uv[c] = vUV[c] * texdims[c] * 128`  (texdims = the texmap mapped to texcoord c).
2. Indirect lookup: `tempcoord = fixpoint_uv[indcoord] >> coordScale` (GX_ITS_* = log2 shift S/T);
   `iindtex = sampleTexture(indmap, tempcoord).abg` (NOTE the **.abg** swizzle: S=tex.a,T=tex.b,U=tex.g).
3. `iindtevcrd = iindtex >> fmt_shift` (ITF_8→0, ITF_5→3, ITF_4→4, ITF_3→5).
4. bias add per bias_sel (ITB_S/T/U/ST/...): add `bias_add` to selected components;
   `bias_add = (fmt==ITF_8) ? -128 : 1`.
5. matrix (Indirect type, ITM_0..2): mantissa `m = round(1024*offset) & 0x7FF` sign-extended 11-bit
   (GXSetIndTexMtx: `(int)(1024*offset)&0x7FF`). `indtevtrans = int2(idot(m_row0,crd), idot(m_row1,crd)) >> 3`;
   then `>>= w` if w>=0 else `<<= -w`, where **w = -scale_exp** (Dolphin: `17 - (scale_exp+17)`).
   Net offset (texel·128 units) = `128 * (offset_row·crd) * 2^scale_exp`.
   - S/T matrix variants (ITM_S*/T*): `indtevtrans = int2(fixpoint_uv[c] * crd.xx_or_yy) >> 8` then scale.
6. wrap regular coord: ITW_OFF→fixpoint_uv as-is; ITW_256/128/64/32/16→`& ((N<<7)-1)`; ITW_0→0.
7. `tevcoord = wrap + indtevtrans` (or `+=` if mPrev/addprev); `tevcoord = (tevcoord<<8)>>8` (s24).
8. regular sample at `tevcoord / (texdims*128)` (= sampleTexture divides by texsize*128).

mMtxSel decode (GXIndTexMtxID): 0=OFF; 1-3=ITM_0..2 (Indirect, mtxidx=sel-1); 5-7=ITM_S* (S,
mtxidx=sel-5); 9-11=ITM_T* (T, mtxidx=sel-9).

## Port plan
1. NgxTevState.ind (NgxIndirect): num_stages, order coord/map[4], coordscale S/T[4],
   mtx mantissas[3][2][3]+exp[3], per-TEV-stage NgxIndTevStage[16]. In the key hash.
2. capture: read IndBlock@0x2C (order/mtx/coordscale/stagenum) + mIndTevStage@variant. Mantissa
   quantization in a pure helper (ngx_indirect.h) + a render_test unit locking the encoding.
3. tev_shader.cpp: before the regular tex sample in write_stage, if this stage's ind is enabled,
   emit the indirect warp (transliterate Dolphin's math) → sample at warped coord. Verified by
   ab_oracle + screenshot (GLSL, like the combiner — no C++ unit for the warp itself).
4. Liveness counter (materials whose shader emitted an active indirect stage).
5. Verify: render_test green, screenshot no-garble, ab_oracle no-regression.

## DONE + verified (this session)
Implemented the faithful Indirect-matrix path (GX_ITM_0..2). Files:
- `runtime/ngx/ngx_indirect.h` (new) — pure helpers: `ngx_ind_mtx_mantissa` (float→S2.10), 
  `ngx_ind_mtx_decode` (GXIndTexMtxID→idx+kind), fmt_shift/bias. Unit-tested.
- `runtime/ngx/ngx_render_data.h` — `NgxIndirect`/`NgxIndTevStage` added to `NgxTevState` (in the key).
- `runtime/overrides/ngx_j3d_shape.cpp` — `capture_indirect()` reads IndBlock(@0x2C) + mIndTevStage
  (TevBlock variant offset, added to TevLayout). `/ngxshape` IndBlock+applied counters.
- `runtime/render/tev_shader.cpp` — `emit_indirect_warp()` transliterates Dolphin's fixpoint math
  into the generated GLSL; the per-stage tex sample uses the warped UV when active.
- `runtime/render/render_test.cpp` — `indirect` unit (mantissa/decode/fmt — all hand-computed GX).

Verification (all gates from the handoff):
- `render_test`: 12/12 PASS incl. new `indirect` (1 hand-calc was wrong — 1024*0.999=1022.976→1022
  trunc, NOT 1023; the function was right, fixed the test).
- `/tevshader`: live materials compile 14/14 (no GLSL errors from the indirect emission).
- `/ngxshape` plaza: `indirect applied (shader-active)=5089`, `S/T-matrix stages skipped=0` ⇒ the
  warp is genuinely exercised AND my Indirect-only path covers 100% of plaza's indirect usage
  (no S/T variants present). max_stages=1.
- Screenshot (`scratch/screenshots/ind_after.png`): plaza renders faithfully — floor/pillar/emblem/
  Mario/HUD/buildings, no garble, no black. (Magenta NPC blob = the separate known matColor bug.)
- `ab_oracle scratch/freeroam_plaza.sav` = **17.1%** mean delta (baseline ~18.3%) — NO regression
  (right-column 84-104 = the parked sun-glare/wash region). Per directive, not chasing the number.

NOT done (documented gaps, counted so a regression would surface): S/T indirect-matrix variants
(0 in plaza), bump-alpha (mAlphaSel), LOD bias (mLod). num_stages>1 is supported by the code but
plaza never exercises it (max_stages=1).
