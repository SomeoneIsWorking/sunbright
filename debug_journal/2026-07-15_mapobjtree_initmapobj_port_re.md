# 2026-07-15 — TMapObjTree::initMapObj port: RE complete, ready to execute

The boot's gameplay frontier blocker (`SB_STAGE=1` OSPanics at
sms-boot/boot_stubs/ring3_stubs.cpp: TMapObjTree::initMapObj not ported). Fully RE'd
via the new port-kit (tools/re/port_dossier.py) + tools/dol_sda.py. Full working dossier:
scratch/re/mapobjtree_port_dossier.md (873+ lines: disasm, class layouts, callers).

## What it does (US 0x801f68b4)

`TMapObjTree::initMapObj` = TMapObjGeneral::initMapObj() (decomp body exists,
MapObjInit.cpp:11204) + `initEach()` + build a per-tree LEAF-COLLISION array:
`new leaf[mLeafCount]` (0x3c-byte elements), each holding a `TMapCollisionMove*` (+8) and a
Mtx (+0xc) copied from the model's joint matrices; names each "/mapObj/palmLeaf%02d" (palm
species) or "/mapObj/%sLeaf%02d". `initEach` (0x801f6a64) and the element ctor (0x801f6ef4)
are DOL-only (no decomp body); TMapCollisionMove's ctor has a decomp body
(MapCollisionEntry.cpp:294).

## Resolved constants (were the missing pieces)

initEach = switch on mActorType (field 0x4c; values 0x40000034..0x40000039 = 6 tree
species) → sets leaf count (field 0x150 = 8 or 12) + f32 params. SDA2 pool
(_SDA2_BASE_=0x80416ba0), via `tools/dol_sda.py --sda2 <off>`:
20.0(-0x2040) 95.0(-0x203c) 0.001(-0x2038) 0.006(-0x2034) 0.01(-0x2030) 0.97(-0x202c)
100.0(-0x2028) 50.0(-0x2024) 60.0(-0x2020) 70.0(-0x201c) 0.004(-0x2018).

## Port plan (cold-RE transcription = main session; build+verify = delegate)

1. Add TMapObjTree fields (0x148/14c f32, 0x150 s32 mLeafCount, 0x154 leaf* , 0x15c-168
   f32×4) + a 0x3c leaf-element struct {f32…, TMapCollisionMove* @8, Mtx @0xc}.
2. Port initEach (species switch above) + element ctor (0x801f6ef4).
3. Port initMapObj (base + initEach + per-leaf loop; collision vtable slots [8]/[0x18]/
   [0x20] + setAllData @0x8018e170 — cross-check TMapCollision{Move,Base} in reference/sms).
4. Delete the ring3 OSPanic; spec test = leaf count per species (8/12); delegate the
   `cmake --build … && SB_STAGE=1 boot` verify to an agent.

Coupled (initEach sets the count initMapObj sizes the array from), so no clean partial
landing — execute as one focused pass from the dossier. No further extraction needed.

## Tooling landed this session (makes future ports one-liners)

- `tools/re/port_dossier.py <name|0xADDR>` — auto disasm(bounded)+decomp-body-check+stub-site.
- `.claude/commands/patch-func.md` — native ONE-RUNTIME port recipe (replaced dead recomp skill).
