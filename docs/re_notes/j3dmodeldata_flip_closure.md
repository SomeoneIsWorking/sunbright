# J3DModelData field-access flip is BLOCKED by object-graph closure (2026-06-15)

**Status: the link/seam infrastructure landed (commit 133b247) is good and kept. But the
`SUNBRIGHT_ENGINE_TYPES=J3DModelData` field-access RECOMPILE is NOT viable as an isolated first
flip — verified, with a concrete root cause. This reframes the first-flip strategy.**

## What was done (kept, committed)
- The PC-native `port/` engine is linked as a GUEST SLICE into the `sunbright` binary
  (`add_subdirectory(port, SMSPORT_BUILD_MAIN=OFF)`); the decomp's global `operator new/delete`
  hijack is gated off (`SMSPORT_GUEST_SLICE`); the full binary LINKS clean with the J3DModelData
  loader closure pulled in and global `operator new` resolving from libstdc++ (no JKRHeap hijack).
- The identity seam is wired: `runtime/overrides/j3d_loader_bridge.cpp` overrides
  `J3DModelLoaderDataBase::load` @0x802e6f00 → `port/bridge/j3d_bridge.cpp` `sbport_j3d_load`
  (copy guest BMD → `bmd_swap_to_host` → pristine port loader → host `J3DModelData`) → returns a
  32-bit `sb_eng_handle`. Gated behind the `SB_FLIP_J3DMODELDATA` build option (default OFF).

## The blocker: who reads J3DModelData fields, and what those fields point to
`SUNBRIGHT_ENGINE_TYPES=J3DModelData` recompile (1.8 s, J3DModelData resolved, 0 missing types)
emits **92 `sb_eng_host` host-field sites + 52 `sb_host_to_guest` pointer-field reads + 2
`sb_set_guest_ptr`** across **26 functions**. Mapping the 26 to names (reference/sms_gmse01_funcs.txt):
they are **ALL engine code**, and every one TRAVERSES the J3DModelData object graph:
- `J3DModelData::{setMaterialTable, isDeformableVertexFormat, entry/remove/set*Animator}` (802dd2dc..),
- `J3DModel::entryModelData`, `J3DSkinDeform::initMtxIndexArray`, `J3DAnm*::searchUpdateMaterialID`,
- `SDLModelData::ct`, `SampleCtrlModelData::ct`,
- the game-adjacent `SMS_DrawShape / SMS_SettingDrawShape / SMS_ResetDamageFogEffect /
  SMS_ChangeTextureAll`, `TScreenTexture::replace`.

The emitted code for a graph-pointer field is BROKEN two ways:
1. `cpu.gpr[5] = sb_host_to_guest((void*)(((J3DModelData*)sb_eng_host(h))->mMaterials));`
   `mMaterials` is a `J3DMaterial**` the port loader allocated with `new J3DMaterial*[]` — a HOST
   buffer NOT inside the guest RAM arena. `host_to_guest_ea` (memory_bridge.cpp) returns **0** for
   any host pointer outside `[g_ram_base, +0x1800000)`. So the field reads as 0 → the recompiled
   engine code then dereferences 0 → null fault.
2. `cpu.gpr[3] = (u32)(((J3DModelData*)sb_eng_host(h))->mVertexData.mVtxAttrFmtList);`
   an embedded-struct host pointer truncated to u32 → wild.

Scalar fields flip CORRECTLY (`sb_eng_host(h)->mJointNum`, inline getters); only the **graph/pointer
fields** are unsound.

## Why this is fundamental, not a recovery gap
The data-boundary "guest-data pointer field" translation (commit 1fca757) is sound ONLY when the
pointee lives in GUEST RAM — e.g. `JUTTexture::mTexInfo` → a `ResTIMG` asset header copied into guest
RAM (`sb_host_to_guest` maps it correctly). J3DModelData is the OPPOSITE shape: a host object whose
non-scalar fields point to **host-constructed engine sub-objects** (material/joint/shape arrays, the
J3DMaterialTable, name tables). Those are never in guest RAM, so the host↔guest translation has
nothing to translate to. You cannot flip J3DModelData's field access in isolation because every
consumer is engine code that walks the host graph as if it were guest memory.

The handoff's "J3DModelData: 92 / 0 — CLEAN, simplest first flip" was about field RECOGNITION
coverage (every offset is named, 0 unmapped) — NOT runtime soundness of the resulting pointer-field
code. The de-risk proved recognition; integration surfaces the graph-semantics gap (exactly the
"gated on SYSTEMS INTEGRATION" caveat).

## Strategic consequence — the recompiler field-access flip fits a DIFFERENT shape
- **Field-access flip (recompiler `SUNBRIGHT_ENGINE_TYPES`) suits a host engine object whose
  non-scalar fields point to GUEST DATA (assets) or are scalars** — the JUTTexture shape
  (mTexInfo/mTexData → guest ResTIMG/texel data). It is the WRONG tool for a host object GRAPH.
- **A host object graph (J3D model/material/shape/joint, J2D, JKR) must be OWNED in `port/` as a
  subsystem**, with the recompiled game holding only the top-level HANDLE and calling a SMALL set of
  BRIDGED engine entry points (load [done], createModel, draw, setAnm…). The internal graph stays in
  `port/` and is never exposed to recompiled code as guest pointers. This is the function-call half
  of the boundary (bridge), applied to whole-subsystem API — not per-field recompiler flipping.
  Note: bridging an engine method whose ARGS are other engine objects (setMaterialTable takes a
  J3DMaterialTable*, entryMatColorAnimator takes a J3DAnmColor*) pulls those types into the closure
  too — the J3D closure is irreducible, so J3D is a SUBSYSTEM-sized unit, not a thin slice.

## Recommended next-session paths (pick one — this is an architecture fork)
1. **Own J3D as a subsystem in `port/`** behind a small bridged API (load → createModel → draw).
   Correct per ARCHITECTURE_TARGET, but large: needs J3DModel/draw + GX owned in `port/` (GX is
   currently 74 no-op stubs) before anything is VISIBLE. The load bridge is step 1.
2. **Re-target the field-access flip to a JUTTexture-SHAPED type** (host object, pointer fields →
   guest data, scalar methods) to validate the recompiler-flip pipeline end-to-end on the shape it
   actually fits. JUTTexture itself is still blocked by the polymorphic-inlined-ctor case
   (J2DWindow::Texture) AND needs GX; survey for a smaller asset-data-backed engine type that has
   neither blocker (programmatic/scalar state, no host sub-object graph).
3. **Find a programmatic-only engine type** (default-constructed, scalar/guest-data state, no host
   graph, no polymorphic-inlined-ctor) for a pure mechanism-validation flip — proves
   link+bridge+handle+flipped-field+recompile+oracle on the smallest possible surface, no GX needed.

The load bridge + link infra (133b247) is reusable for all three.
