# Delfino floor wash — CORRECTION: mipmaps FALSIFIED too. Wash still OPEN. (2026-06-18, session 14b)

I (session 14) committed `2026-06-18_floor_wash_IS_missing_mipmaps.md` claiming the wash = ngx not
loading the floor texture's authored DARK high mips. **That is WRONG.** Read this correction.

## Why it's wrong (hard data)
The floor texture (0x80d32940) has **ResTIMG.mipmapCount = 4** (verified live: `/gxstate?ti=10` →
`mipCnt=4`; ResTIMG layout from `reference/sms/include/JSystem/ResTIMG.hpp` — mipmapCount@0x18,
minFilter@0x14). Its 4 official levels (L0 64×64 … L3 8×8) are ALL bright cream (~235,227,218,
hand-decoded CMPR endpoints). The "dark blue L4/L5/L6" I decoded earlier live at byte offsets BEYOND
this texture's 4-level chain (2720 B) — that is the **NEXT texture's data**, not the floor's mips.
So GX samples 4 bright mips → the floor texture contributes BRIGHT, same as ngx. Mips are not it.

Independently confirmed: I implemented authored-mip loading in `vk_mesh.cpp` (decode all
ResTIMG.mipmapCount levels at block-padded size, upload per-level, trilinear sampler) and the
frame-exact `oracle_ab.sh 14` A/B was **byte-identical** to baseline (floor rows 3-4 still ~120-137,
mean 89.24 vs 89.25). The mip fix is KEPT (ngx was wrongly sampling only L0 in general — a real
correctness gap, listed in memory `ngx-n6-lighting`), but it does NOT change the Delfino frame and is
NOT the wash fix. (Lesson: I concluded "authored dark mips" from adjacent bytes WITHOUT first reading
mipmapCount — exactly the "build on a deduction" trap the handoff warned about. Read the count first.)

## What is now RULED OUT for the floor wash (all hard data)
floor: GX≈(101,106,112) blue-grey ~0.42×; ngx≈(250,250,250) white. The floor shape (8112bfdc, ti=10)
is the SAME winner in both engines (pixbatch). All ruled out:
- **Cast shadow** (session 13's lead): `SUNBRIGHT_KILL_SHADOW` → tiny blob under Mario, ~3 of ~120.
- **Vertex color**: CLR0 idx=1=WHITE (re-parsed the real DL, 138/159 verts; array 80b9b600[1]=white).
- **Texture + mips**: CMPR, all 4 authored mips bright cream (~235). Decode parity PASS.
- **TEV combiner**: 08f8af = tex×rasColor (GX bpmem async-settles to the SAME value, PASS).
- **Dynamic lighting**: floor shape has NO normals (nrm cls=0); block is CLOF (en=0).
- **Fog**: GXSetFog sync tee = GX_FOG_NONE.

## The crux (unresolved) + the best lead
`frag = texture(bright ~235) × rasColor`. For GX to land on (101,106,112), rasColor must be ~0.42 blue
— yet the material's color block is **J3DColorBlockLightOff, cc=0x0701, enable bit1=0 = UNLIT**
(decomp `J3DColorChan` bit layout, and `J3DColorBlockLightOff::load` programs `GXSetChanCtrl(en=0)`).
An unlit channel's output = matColor source = vtxColor = WHITE → ngx is "correct" → bright. So GX is
darkening by a path OUTSIDE the per-material object state ngx reads. Candidates for next session:
1. **Lit-with-ambient after all (STRONGEST lead).** A `SUNBRIGHT_NGX_AMBMUL` probe (multiply the
   UNLIT output by the live GXSetChanAmbColor register ~(128,128,128)) dropped the floor 235→**128 ≈
   GX 105**. xfmem cc flickered en=1 (mask=03) ~2/3 reads. POSSIBLE that GX renders these "LightOff"
   materials LIT with the inherited global ambient (J3DColorBlockLightOff::load does NOT set ambient —
   it inherits the last LightOn material's GXSetChanAmbColor), no diffuse (no normals) → output =
   matColor × ambient ≈ ×0.5. If so the bug is: ngx reads the static block (en=0) but the GPU's
   effective channel control enables lighting. RESOLVE by finding where the floor's GXSetChanCtrl gets
   en=1 (material anim? a global enable? mis-association of the block to the shape?). The AMBMUL probe
   was removed (it tested this); re-add to A/B. Also: the true ambient is BLUER than (128,128,128).
2. **A transparent TINT overlay draw** ngx z-culls. pixbatch shows ti=9 (sky-blue, blend=1) and ti=78
   (blend=1, α0.30) fragments BEHIND the floor; if GX draws a blue tint over the floor without z-test,
   ngx (which z-tests) drops it. Check immediate-mode / no-ztest overlay draws over the plaza floor.
3. **EFB→XFB copy scale/gamma** specific to the 3D scene (file-select is faithful, Delfino is not). It
   is a ~0.42 LINEAR multiply (not gamma: 0.98→0.40 needs gamma ~45). Check GXSetDispCopyGamma / a
   fullscreen multiply pass vs ngx_present.

pillars/trees are ALSO ~2.4× darker in GX (green, lit); the wall only ~1.3×. So the darkening hits the
big 3D env surfaces. file-select has NO wash. → scene/3D-specific, hits floor+trees most.

## Tools (reliable)
`tools/render/oracle_ab.sh 14` (SUNBRIGHT_BIN=build-freshtest/sunbright); `SUNBRIGHT_KILL_SHADOW=1`
(shadow_kill_diag.cpp); `/gxstate?ti=10` (now prints `mipCnt`); hand CMPR decoder over `/r`.
ALWAYS read mipmapCount/struct fields before concluding — don't deduce from adjacent bytes.
