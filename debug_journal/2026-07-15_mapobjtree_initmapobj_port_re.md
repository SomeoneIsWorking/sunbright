# 2026-07-15 — TMapObjTree::initMapObj port: LANDED + verified

**STATUS: DONE.** Ported into `reference/sms/src/MoveBG/MapObjTree.cpp` +
`include/MoveBG/MapObjTree.hpp`; `ring3_stubs.cpp` OSPanic removed. Unit test
`mapobjtree_initeach_test` PASSes (species table). Whole-system: default-fastboot (Delfino)
no longer OSPanics at initMapObj — boot advances through object-gen, perform-list load, and
render, then aborts on an UNRELATED aurora `GXBegin without matching GXEnd` (render layer).
Next boot frontier: `TMapObjTree::perform` (loud stub) + that GXBegin/GXEnd mismatch.


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

## EXECUTED 2026-07-15 — the remaining unknowns, resolved

The port was transcribed into `reference/sms/src/MoveBG/MapObjTree.cpp` (was an empty
decomp file; it's already in the CMake glob) + `include/MoveBG/MapObjTree.hpp` fields, and
the `ring3_stubs.cpp` OSPanic deleted. Four things the dossier left open, now closed off
the DOL:

- **0x40000039 arm growth constants** (dossier stopped at -0x2018): -0x2018=0.004,
  -0x2014=0.008, -0x2010=0.03, -0x200c=0.9. Full species table (mActorType → leafCount,
  unk148, unk14C, then the 0x15c/160/164/168 quad):
  - 0x34 & 0x38(palm) [share arm 0x801f6aac]: 12, 20, 95, {0.001,0.006,0.01,0.97}
  - 0x35: 8, 20, 100, {0.001,0.006,0.01,0.97}
  - 0x36: 12, 50, 100, {0.001,0.006,0.01,0.97}
  - 0x37: 8, 95, 60, {0.001,0.006,0.01,0.97}
  - 0x39: 8, 70, 100, {0.004,0.008,0.03,0.9}
  - out of 0x34..0x39 → no-op (blr/bgelr).
- **Leaf element (0x3c, ctor 0x801f6ef4)** = `{f32 @0, f32 @4, TMapCollisionMove* @8,
  Mtx @0xc}`. The ctor's `1.0`s at 0xc/0x20/0x34 are the Mtx DIAGONAL → it's
  `PSMTXIdentity(mMtx)`; +8 gets a `new TMapCollisionMove`. initMapObj then re-news +8
  (benign one-time retail leak — reproduced faithfully, not "fixed").
- **Glue bl targets**: 0x802c3ba4/0x802c3ca4 = operator new/new[] (r13-0x5f2c allocator
  thunk) → native `new`/`new[]`. 0x8009544c = a Mtx copy (dst=r3,src=r4) → the joint
  copy; done as `PSMTXCopy(getModel()->getAnmMtx(mLeafCount-i), leaf->mMtx)`.
  getAnmMtx(idx) IS the `model+0x58` (mNodeMatrices[idx]) access. Indices run
  mLeafCount..1 (joint 0 = root, skipped).
- **Collision vtable** (the trap): the Move ctor stores TWO vtables — base 0x803c1744
  (inlined base ctor) then the REAL Move vtable 0x803c16fc. Against 0x803c16fc:
  byte+8 = 0x8018dc10 = **Move::init** (so unqualified `mCollision->init(name,0,this)` is
  faithful), byte+0x18 = 0x800969ec = base **setUp** (`mFlags@0x5c &= ~1`), byte+0x20 =
  0x8009eb8c = base **remove** (`mFlags |= 1`). Loop order: init → setAllData((s16)i) →
  remove → copy joint mtx into leaf+collision → setUp. Final: if `mMapCollisionManager`
  (TLiveActor 0xec) non-null, its `unk10`(0x10, `const TLiveActor*`) = null.

Verification: unit test `sms-boot/runtime/tests/mapobjtree_initeach_test.cpp` pins the
species table (drives the real linked initEach over a zeroed buffer — non-virtual,
POD-only). Whole-system: default-fastboot (Delfino) must clear the old OSPanic.

Coupled (initEach sets the count initMapObj sizes the array from) — landed as one pass.

## Tooling landed this session (makes future ports one-liners)

- `tools/re/port_dossier.py <name|0xADDR>` — auto disasm(bounded)+decomp-body-check+stub-site.
- `.claude/commands/patch-func.md` — native ONE-RUNTIME port recipe (replaced dead recomp skill).
