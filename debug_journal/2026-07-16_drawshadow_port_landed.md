# 2026-07-16 — faithful Z-stencil drawShadow PORTED; runs stable; masking residual open

The TMBindShadowManager dst-alpha volume-shadow pipeline (RE map:
2026-07-16_drawshadow_re_map.md) is now fully ported and runs end-to-end at
title/file-select (models load, calcVtx clusters, both draw phases fire per frame,
no crashes over 120 s turbo runs). The decal simplification is gone. Landing this
flushed out FOUR pre-existing landmines, each with its own fix:

## 1. `sphere_glist_p = &tmp_data` — a silent scaffold that replayed .bss as GX commands

`TModelWaterManager::drawShineShadowVolume` pointed its "sphere display list" at a
1-BYTE dummy static and called `GXCallDisplayList(ptr, 0x760)` — 1888 bytes of
adjacent .bss decoded as GX commands. Dormant only while the linker happened to put
zeros (GX NOPs) there; ANY layout change (this port's new statics) turned it into a
command-processor desync ("unknown opcode 0xE0" — the mystery bytes were two
LITTLE-ENDIAN static-init pointers that landed after tmp_data). Now a LOUD one-shot
`[STUB-CALLED]` + skip; the real fix needs retail's baked GXDrawSphere DL
(SHINE shadow volume) — RE-frontier debt. Found via SB_DL_VALIDATE's caller
backtrace after the new overflow-fatal draw-ring narrowed the frame.

## 2. VcdVat DLs: never baked, garbage tails, and an aurora-sized window

- J3DShapeFactory's `mpVcdVatCmdBuffer` was never zeroed: `GXCallDisplayList`
  replays a FIXED window per shape but `makeVcdVatCmd` writes less — the tail
  must be GX NOPs (0x00), not heap garbage. Now memset at alloc.
- On this port `makeVcdVatCmd` is never called by the loader (J3D packet draws go
  through the native capture hook). The shadow path draws shapes RAW
  (SMS_SettingDrawShape + J3DShapeDraw geometry DL replay), so
  TMBindShadowManager::load() bakes the VcdVat DL per shape explicitly.
- `kVcdVatDLSize` grew 0xC0 → 0x180 on this platform: Aurora bakes 64-bit
  host-pointer array bases (GX_AURORA_LOAD_ARRAYBASE, ~15 B/attr) vs GC's 6-byte
  CP writes. All replay sites now use the constant (J3DShape::draw,
  makeVcdVatCmd's GDInit, SMS_SettingDrawShape — the latter two had the retail
  0xC0 literal).

## 3. Staging buffer had 0.3% headroom

The title/file-select frame already used 33.44 MB of the 32 MB storage staging
region (the ghost-pass double-draw wart, journal 2026-07-15) — the shadow passes
tipped it over. Grown to 48 MB; the ghost-pass wart REMAINS the real fix. The
overflow fatal now prints the last 16 draw identities (prim/fmt/verts/mark) so the
next capacity event self-diagnoses.

## 4. Port-seam guards the retail code never needed

- calcVtx: first flag-4 perform can precede the map collision load on this port
  (retraces advance with load wall-time; timing-dependent SIGSEGV in checkGround).
  Guard: gpMap/mCollisionData/grid tables non-null.
- perform flag-4: retail normalizes the light-pos global @r13-0x6110
  unconditionally; the port maps it to TLightWithDBSetManager::mEffectPos which is
  ZERO until the light manager runs → VECNormalize(0) = NaN shadow dir → NaN
  request positions → checkGround grid OOB (the "crashes only outside lldb"
  heisenbug — ASLR/timing changed which frame hit it). Guard: keep straight-down
  until a real position exists.
- drawShadow exit: retail leaves ColorUpdate(0) and relies on the NEXT buffer's
  ReInitializeGX to restore it; our captured-J3D buffers don't interleave that
  call, so everything after the shadow node rendered invisible (A/B/C blocks
  vanished). Host seam: restore ColorUpdate(1) at exit (same observable state as
  retail's re-init).

## Ported surface (reference/sms 9afe5138+)

ShadowUtil.hpp/cpp: retail structures (TAlphaShadowQuad footprints, blend-quad
cluster boxes, groups, prism vtx pool, type-2 side channel), ctor/load (loads
/common/shadowCircle|shadowCircleLow|shadowCube|ShipShadow.bmd via
getGlbResource + J3DModelLoaderDataBase::load(0x10210000)), request/forceRequest
(dist²-to-Mario + flags now stored — LOD selection input), calcVtx (type-1 light
projection, water re-probe, TRS matrix via MsMtxSetTRS + view concat, corner
quads, conectCubeDiffer/Same clustering), drawShadow (5-pass dst-alpha stencil),
drawShadowVolume (prism caps + 5×4-triangle walls + LOD models), perform
(flag 4/8/0x20000000 routing). SMS_DrawCube (DrawUtil) transcribed from
0x80225d00 (was an EMPTY silent stub — the alpha-stamp emitter).

## OPEN residual: volumes darken their full coverage (mask not constraining)

At file-select the blocks are darkened by their own enclosing volumes and Mario's
feet shadow isn't visibly masked to the ground yet. Retail avoids this via the
draw-ORDER classes: perform is called twice per frame (flags 0x4000000e then
0x20000008) and group masks (0x20000000/0x40000000 from request flag bit 30)
split shadows across those two calls, interleaved with object buffers so casters
draw OVER their own volume darkening. Our pipeline's interleave differs — next
step: dump which buffers run between the two shadow perform calls vs retail's
perform list, and check the EFB pixel-format/dst-alpha semantics in aurora for
the RGBA6 assumption. Diagnostics: SB_SHADOW_DBG (counts), SB_SHADOW_BISECT
(1=skip all, 2=setup only, TEMP — remove when the residual closes).
