# 2026-07-16 — TMBindShadowManager Z-stencil shadow: complete RE map (pre-port)

RE of the retail volume-shadow subsystem (ShadowUtil, decomp gap — upstream doldecomp/sms
ShadowUtil.cpp is EMPTY as of 4fa80205, so this is a hand-port). Decompiles in
`scratch/decomp_shadow/*.c` (regen recipe in 2026-07-16_vertical_framing_resolved.md).
Goal: replace the simplified decal `drawShadow` in reference/sms/src/MarioUtil/ShadowUtil.cpp
(RE-frontier hack) with the faithful dst-alpha-stencil implementation.

## Manager guest layout (ctor FUN_802313e4 @0x802313e4 — in the US symbol gap)

| off  | contents |
|------|----------|
| +0x10 | TCircleShadowRequest[0x200] (stride 0x24; ctor LAB_80231680) |
| +0x14 | request count (reset by perform flag 0x20000000) |
| +0x18 | footprint array [0x200] stride 0x70 (see below) |
| +0x1c | group array [0x100] stride 0x14: {u32 mask; fpHead; fpTail; boxHead; boxTail} |
| +0x20 | group count |
| +0x24 | box array [0x200] stride 0x20: {minX,y,minZ,maxX,dy,maxZ,next?,0} |
| +0x28 | special-quad array [0x1e] stride 0x3c = 5 xyz corners (prism footprint) |
| +0x2c | special-quad count (cap 0x1d) |
| +0x30 | Vec shadow dir = normalize(dir from obj @r13-0x610c via FUN_802281b8), set each perform flag-4 |
| +0x3c | J3DModelData** [5] models: [0]=circle LOD0, [1]=circle LOD1, [2]=body prism fallback, [3]=type-3 disc (idx*4: 0,4,8,0xc used) |
| +0x40 | u16 (reset 0) |
| +0x49 | byte: set 1 after draw-reset, 0 at calc |
| +0x4c | byte local_c — UNINITIALIZED in retail ctor (benign UB, document) |
| +0x54 | intrusive TList<TMBindShadowBody*> head/tail (self-init) |
| +0x5c | GXColor shadow color: default (0x1e,0x32,0x73,0xb4); stage 7: (0x2d,0x28,0x3c,0x5a); stage 6: (9,9,0x1c,0x74) — stage byte DAT_803e970e |
| +0x60 | f32 = SDA2[-0x1620] = 30.0 (raycast up-offset, used @calcVtx as mgr+0x60) |
| +0x64 | byte debug-extra-pass flag |
| +0x65 | byte (reset each frame) |
| +0x68 | f32 = SDA2[-0x1670] = 0.5 (aspect ratio clamp lo) |
| +0x6c | f32 = SDA2[-0x161c] = 1.55 (aspect clamp hi mult) |
| +0x70 | single scratch group (0x14 × 1) |

gpBindShadowManager = r13-0x6100. gpMap = r13-0x6328. r13-0x7708 = 200.0, r13-0x7704 = 0.02,
r13-0x7700 = 20.0 (f32 globals @0x8040cab8.. — calcVtx probe/box constants). Body-entry s16→deg
conv: SDA2[-0x1604] = 0.00549316 (=360/65536) with the 0x43300000 double-conv idiom. r13-0x60f7: byte selects drawShadowGD(1)/drawShadow(0) in perform.
r13-0x60f8: byte selects drawShadow's debug fullscreen branch (0 = real path).

## Footprint record (stride 0x70, floats indexed)

[0]=volume size (max(radX,radZ) clamped by 200, ×1.1); [1..0xc]=MODELVIEW mtx =
PSMTXConcat(DAT_804045dc(global view mtx, BSS — resolve writer via xref), local mtx from
FUN_8022a8e8(pos, ?, alpha?, radX, radZ) — mtx builder to transcribe); [0xd..0x18]= 4
ground-corner verts, corner k: (x + radX·dirX[k], groundY, z + radZ·dirZ[k]) with
dirX=(-1,1,1,-1) dirZ=(-1,-1,1,1) (DAT_8039db90/dba0); [0x19]=ptr to special-quad (or 0);
[0x1a]=back-ptr to request; [0x1b]=cluster-link (next fp in group, +0x6c in bytes).

## calcVtx (FUN_8022e0cc) flow

Per request (type byte +0x1c: 0=circle, 1=body/prism, 3=fixed):
1. type 1: sun-project the CENTER along mgr shadow dir (+0x30 x, +0x38 z slopes):
   pos = 0.5·(proj(top)+proj(bottom)) per axis (top = y + r13[-0x7708]).
2. if grounded-flag (+0x1d): y = gpMap->checkGround(x, y+mgr[+0x60], z); if surface type in
   {0x100,0x101,0x102..0x105,0x4104} (water/lava?) → checkGroundBelowWater (FUN_801898f4).
3. radii: radX·=0.08·(various), type3: fixed 0.2/etc (constants 0.5,1,2e7,0,0.2,90,0.08,
   0.8,0.01,200,1.1,-1 — see dol_sda dump of 0x8022e0cc; transcribe exactly from decompile).
4. type 1 close-to-ground (|projY−groundY|<1.0 and specialCount<0x1d): emit special-quad =
   5 corners (slanted prism footprint: 4 offset corners + center variants — transcribe the
   two sub-branches exactly), fp[0x19]=&quad.
5. fp matrix + 4 corners built as above.
Then clustering (skipped when r13-0x60f8 debug): reset groups; per fp make box
{minX=cx−?, y, minZ, maxX, dy=r13[-0x7700], maxZ} (+0x24 recs); test against each group's
box-list via FUN_80230fac (box overlap) → append fp to group's fp-list (link +0x6c) and box
to box-list (+0x1c); else new group, mask=0x20000000 (0x40000000 if request flags +0x20 bit
30). Then pairwise group merge via FUN_80230e68 (list-vs-list box overlap) concatenating
lists and OR-ing the 0x40000000 mask.

## drawShadow (FUN_8022f014 @0x8022f014) — the dst-alpha stencil (real path)

Common setup: ReInitializeGX; GXSetZCompLoc(1); ClearVtxDesc; VtxDesc(POS,DIRECT);
VtxAttrFmt(fmt0, POS, XYZ, F32, 0); NumChans(1); ChanCtrl(COLOR0/COLOR1/ALPHA0: off,
src=REG…,DF_NONE=2); NumTexGens(0); NumTevStages(1); TevOp(0, PASSCLR=4);
TevOrder(0, 0xff, 0xff, GX_COLOR_NULL=4); AlphaUpdate(1); ChanMatColor(GX_COLOR0A0=4,
mgr color +0x5c); SetCurrentMtx(0); GXLoadNrmMtxImm(g->mViewMtx (+0xB4), 0).
Per group (mask & drawFlags, both lists non-empty), LOD pick: bVar9 = (2e7 <= req->unk18)
— effectively FALSE ⇒ LOD1 selected via SettingDrawShape(models[1]) else models[0]:
1. **alpha stamp**: cull NONE(0); LoadPosMtx(view); ColorUpd(0); DstAlpha(1,0);
   ZMode(1,ALWAYS=7,0); Blend(1,ONE=1,ONE=1,5); box=(bx.minX, bx.y−bx.dy, bx.minZ)…
   (bx.maxX, bx.y+bx.dy, bx.maxZ) from group boxHead; SMS_DrawCube(min,max);
   SettingDrawShape(models[LOD]) primes vtx arrays.
2. **volume mark**: DstAlpha(0,0); ZMode(1,LESS=3,0); cull FRONT? (GXSetCullMode(2));
   Blend(1,ONE,ZERO? (1,1,1..) exact: Blend(1,1,0,5) wait — transcribe: FUN_80361dd0(1,1,0,5)
   after FUN_8035e210(2)); per fp in group: LoadPosMtx(fp mtx +4); drawShadowVolume(mgr,
   !LOD0, fp).
3. **darken**: Blend(1,GX_BL_DSTALPHA=6,GX_BL_INVDSTALPHA=7,5); ZMode(1,GEQUAL?=6,0);
   cull BACK(1); DstAlpha(1,0); ColorUpd(1); same fp loop again.
4. **type-3 extra**: DstAlpha off? (cull 2, ColorUpd 0, ZMode(1,7,0)); per fp with type 3:
   LoadPosMtx(fp); Setting+DrawShape(models[3]).
5. re-setup vtx fmt; LoadPosMtx(view); SMS_DrawCube(same box) — clears the alpha stamp.
6. mgr+0x64 debug: ColorUpd(1); Blend(1,SRCALPHA=4,INVSRCALPHA=5,5); ZMode(1,7,0); DrawCube.
Restore: ZCompLoc(0); ColorUpd(0)→? transcribe tail: GXSetZCompLoc(0), GXSetColorUpdate(0)?
— tail is FUN_80361fcc(0); FUN_80361ed4(0); FUN_80361f14(1); FUN_8036215c(1,0) =
ZCompLoc(0), ColorUpd(0)!, AlphaUpdate(1), DstAlpha(1,0). (Verify against disasm — ColorUpd(0)
at exit looks odd; the next drawbuf re-inits.)

NOTE exact arg transcription must come from scratch/decomp_shadow/8022f014.c + the GX enums —
the pass list above is the shape, not the port source.

## drawShadowVolume (FUN_802305dc)

type==1: if fp->specialQuad: two GX_TRIANGLEFAN(fmt0, 0x12=18 verts) fans (top y+50, bottom
y−50; index order from table @0x8039db48 = [2,1,0,3,2,0,4,3,0, 0,1,2, 0,2,3, 0,3,(4)] —
transcribe exact 18-vert sequences) + GX_TRIANGLEFAN 0x3c=60 verts side walls from the 5
corners ±50 (eps = SDA2[-0x1624] = 50.0). else SettingDraw+Draw(models[2]).
type==3: Setting+Draw(models[3]). else (0): DrawShape(models[LOD]); then
SettingDrawShape(models[other LOD]) — pre-primes next call.

## Models (mgr+0x3c) — procedural J3D blob

Built at static-init (no bl callers → __sinit): FUN_802324a8 (+siblings) serializes a
BIG-ENDIAN J3D model stream into buffer @r13-0x5778 (write-ptr at +8) — sin/cos circle
verts (0xb segments visible), FUN_80232fec(4)=capacity check. Port strategy: replicate the
blob and feed the PORTED J3D loader (BE-swap-on-load already handles it), OR port the
builder to emit J3DModelData directly. The GD path (drawShadowGD @0x8022fa40 + TSetup1..5/
TCylinder statics, dtors 0x80230384..0x80230578) records the same passes into GD display
lists — NOT needed for the port (perform picks immediate path when r13-0x60f7==0; port
pins the immediate path and routes drawShadowGD → drawShadow, documented).

## perform (FUN_80231108)

flag 4: mgr+0x49=0; VECNormalize(FUN_802281b8(obj @r13-0x610c) → dir) into mgr+0x30; calcVtx.
flag 8: drawShadow (or GD per r13-0x60f7); if flags&0x20000000: +0x49=1, request/group/
special counts=0, +0x65=0, +0x40=0.

## Body entry (FUN_80231dd0 — TMBindShadowBody per-joint circle)

Builds a TCircleShadowRequest from joint matrices (radii from per-joint config +0x10/0x14/
0x18 selected by flags +0x15/+0x16), yaw via matan(dz,dx), pos slid along mgr dir slopes
(+0x30/+0x38 = dirX/dirZ over dirY), then mgr->request(). FUN_802319f8 (@0x80231aec grab) =
the caller (entryDrawShadow loop). SDA: r2-0x1604/1600 (s16→deg conv), −0x165c=90.

## Open items for the port

- DAT_804045dc writer (BSS view-mtx global) — Ghidra xref; likely set from camera unk21C copy.
- FUN_8022a8e8 (fp matrix builder) + FUN_80230fac/80230e68 (box tests) — transcribe
  (decompiles in scratch/decomp_shadow/).
- SDA2[-0x1620] (+0x60 offset), SDA2[-0x161c] (+0x6c), r13-0x7708/-0x7704/-0x7700 values —
  dump via dol_sda on the ctor/calcVtx (ctor addr 0x802313e4 for the -0x1620/-0x161c).
- request()'s existing native port (sb::shadow_gate_accept) must be preserved/reconciled.
- SMS_DrawCube is an EMPTY SILENT STUB in DrawUtil.cpp — port it (GXBegin(GX_QUADS,fmt0,24),
  6 faces from min/max — exact vertex order in scratch/decomp_shadow/80225d00.c).
