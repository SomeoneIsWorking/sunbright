# Owning J3D as a port/ subsystem — execution plan (2026-06-15, user-chosen Option 1)

Decision (user, 2026-06-15): the first real flip is **own the J3D subsystem in `port/` behind a small
bridged API**, game holds handles. This supersedes the J3DModelData field-access flip, which is the
WRONG tool for a host object graph (`docs/re_notes/j3dmodeldata_flip_closure.md`).

## The corrected mechanism — FUNCTION-CALL bridges, not field-access flip
The boundary for an object-GRAPH subsystem is the **function call** (the proven SUNBRIGHT_BRIDGE /
override + SB_ENGINE_TYPE handle marshalling), NOT the recompiler `SUNBRIGHT_ENGINE_TYPES`
field-access flip. The recompiled game:
- holds J3D objects only as 32-bit HANDLES (J3DModelData*, J3DModel*, …),
- calls the J3D engine API via `bl <addr>` → override → port-native method on `sb_eng_host(handle)`,
- NEVER field-derefs a J3D object (the host graph stays inside `port/`).
The whole J3D object graph (J3DModelData/J3DModel/J3DMaterial/J3DShape/J3DJoint/J3DAnm*/draw buffers)
is port-native; its inter-references are host pointers that never cross the boundary as guest data.
SB_ENGINE_TYPE is declared once per J3D type that appears in a bridged signature (so `this` and
engine-typed args/returns marshal handle↔host). `SUNBRIGHT_ENGINE_TYPES` field-flip is reserved for
the RARE genuine game-side INLINE read of a SCALAR/embedded-VALUE field (joint matrix floats, counts)
— those flip soundly; graph-pointer reads in game code must instead be a bridged getter.

## API surface the game calls (the bridge boundary) — addresses from sms_gmse01_funcs.txt
- **Load (DONE, 133b247):** `J3DModelLoaderDataBase::load` 0x802e6f00 → host J3DModelData + handle.
- **Construct model:** `__ct__8J3DModelFP12J3DModelDataUlUl` 0x802dde2c (J3DModel(J3DModelData*,u32,u32)),
  `entryModelData__8J3DModel` 0x802ddf90. J3DModel is POLYMORPHIC + out-of-line ctor → use the
  proven placement-new ctor bridge (construct_slice Pattern AV, 5ceb329): at the game's `new J3DModel`
  site emit `sb_eng_alloc<J3DModel>()` + bridged ctor that placement-news the host object (sets host
  vtable). Needs the recompiler object-identity construction emission (object_identity.md) active for
  J3DModel — verify `scratch/identity_sweep J3DModel` first (J3DModel has 1 subclass SDLModel; route
  most-derived per 6be4ac6).
- **Per-frame calc (GX-FREE — first verifiable slice):** `calc__8J3DModel` 0x802debc4 (+
  calcWeightEnvelopeMtx 0x802de7e4, calcNrmMtx 0x802df0f0, calcBumpMtx, calcBBoard, viewCalc
  0x802deeb8). Port `J3DModel::calc()` is present and touches NO GX (pure matrix math) → bridge it and
  verify the computed matrix arrays vs the DISABLE_RECOMP oracle numerically, NO renderer needed.
- **Animation attach:** the J3DModelData `entry/remove/set*Animator` cluster 0x802dd2dc..0x802ddf90,
  `J3DMtxCalc*`. Args are engine objects (J3DAnmColor, J3DAnmTexture…) → SB_ENGINE_TYPE each.
- **Draw (GATED ON GX OWNERSHIP — the large piece):** `entry__8J3DModel` 0x802dedc8 → recursiveEntry
  → J3DShape::draw → GX. port GX is 74 no-op stubs (port/pal/gx/gx_stub.cpp). A VISIBLE frame needs GX
  owned in `port/` (the renderer-ownership effort; runtime/render is the transitional reference).

## ⛔ STEP 0 (PREREQUISITE, found 2026-06-15) — the generated engine-types code does NOT COMPILE
The de-risk validated the flip EMISSION against STUB struct definitions in a harness; the real
compile-into-the-binary path was never exercised, and it is BROKEN two layers deep. Verified by
actually compiling a `SUNBRIGHT_ENGINE_TYPES=J3DModelData J3DModel` generated file:
1. **No struct definitions.** Generated `functions_*.cpp` only `#include "functions.h"` (decls +
   `intrinsics.h`); it does NOT define `J3DModelData`/`J3DModel`. So `((J3DModelData*)sb_eng_host(h))
   ->mMaterials` and `sb_eng_alloc<J3DModel>()` (needs `sizeof`) → "J3DModel was not declared".
2. **Type-system collision if you DO pull the decomp headers in.** Including the port/decomp engine
   headers into the generated TU (to get the struct defs) conflicts with `runtime/cpu_state.h` (which
   the generated TU needs for `CPUState`): `cpu_state.h` `using u64 = uint64_t` (== `unsigned long` on
   LP64 Linux) vs the decomp `types.h` `typedef unsigned long long u64` → hard conflicting-declaration
   error (and likely more macro/type clashes behind it). Two large header worlds can't co-inhabit one TU.

**Consequence — the inline-host-struct emission model is not compile-integrable as-is. Resolve before
ANY flip (this gates J3DModelData AND J3DModel — the load-bridge milestone dodged it only because it
built with the NON-engine-types generated set).** Options:
- **(A) RECOMMENDED for Option 1 — emit BRIDGE CALLS, never host structs in generated code.** A
  recognized construction emits a bridge FACTORY call (alloc+construct in port/, returns a handle);
  an engine field access in GAME code emits a bridge GETTER/SETTER (handle in, scalar/handle out).
  Generated code then uses only `CPUState` + runtime types + opaque `u32` handles — the decomp headers
  never enter the generated TU, the type collision vanishes, and the host graph stays wholly in port/.
  This is the clean separation the architecture wants; cost = recompiler emission rework + generating
  per-field/method bridge thunks (mechanical from the type DB). Engine METHODS are already bridged
  (override at addr), so most "field access" in engine code disappears with the bridge; only genuine
  game-side inline field reads need a generated getter.
  IMPLEMENTATION DETAIL for (A): the current emission is NAME-based (`((T*)sb_eng_host(h))->member`,
  c_emitter.cpp emit_eng_field ~L156) — ABI-CORRECT because the HOST COMPILER computes the offset
  (vtable ptr is 8 bytes on host vs 4 on guest; base-class subobjects; alignment; EBO). Do NOT replace
  it with numeric host offsets emitted as literals unless the type DB provably models the host ABI
  exactly (it currently dodges that by using names) — wrong offsets = silent corruption. Instead, have
  the recompiler EMIT A MANIFEST of needed accessors {type, field, op} + construction sizes, and a
  port-compiled codegen file define stub thunks `sbget_<T>_<field>(void* h){ return ((T*)h)->member; }`
  / `sbset_…` / `sbnew_<T>(args){ return new T(args); }` (real host types, ABI-correct). The emitter
  emits CALLS to those stubs by symbol; the generated TU sees only `extern` decls + handles. For
  construction without a stub-per-callsite, `sb_eng_alloc_sized(<hostSizeof literal>)` works IF the
  type DB's host sizeof is trustworthy (verify vs the port `sizeof(T)`); otherwise a port stub
  `sbsizeof_<T>()`. Keep recomp_test green (it asserts the current `sb_eng_alloc<T>` / `->member`
  emission — update those expectations to the bridge-call form).
- (B) Emit standalone POD struct defs from the type DB into a generated header (no decomp headers) —
  fragile for polymorphic/embedded/base-chain layouts; must stay binary-identical to port.
- (C) Reconcile the type systems via a shim and compile engine-types generated TUs WITH the decomp
  headers+shims — closest to the de-risk intent but mixes two header worlds per TU (brittle).

Until Step 0 lands, the function-bridge surface below can still be BUILT and unit-tested in port/, but
the recomp→bridge dispatch for CONSTRUCTION can't be wired (the construction site needs Step 0).
Engine METHODS that take an already-existing handle (calc, animators) CAN be bridged via plain
overrides today (no generated-code type needed) — but they're only reachable once a host J3DModel
EXISTS, which needs construction = Step 0.

## Execution order (each step commit+push; verify before moving on)
1. **J3DModel construct + calc bridge (GX-free).** Bridge ctor (placement-new) + entryModelData +
   calc family. Declare SB_ENGINE_TYPE(J3DModelData), SB_ENGINE_TYPE(J3DModel). Free-fn wrappers in
   port/bridge (compiled with shims). Register overrides in runtime/overrides/. Build with
   SB_FLIP_J3DMODELDATA-style option (extend to SB_FLIP_J3D). VERIFY: drive the game headless to a
   model load+calc; compare the host J3DModel's joint/weight/normal matrices to the oracle's guest
   matrices via the probe (/r reads guest matrices on the oracle; the native side dumps host). This is
   the first end-to-end "recompiled game drives a port-native engine object" proof.
2. **Animation bridge.** entry/set animators + J3DMtxCalc; verify animated matrices vs oracle.
3. **GX ownership in port/** (the big one) → bridge entry/draw → first NO-DOLPHIN textured frame
   (the MVP gate). Move runtime/render's GX decode into port/ or write port GX over the same Vulkan.

## Landmines (carried forward)
- Construction: J3DModel is polymorphic; only the OUT-OF-LINE ctor (0x802dde2c, there is a `bl`) is
  bridgeable via placement-new. Inlined polymorphic ctors remain unsolved (object_identity.md) — but
  J3DModel's common construction is the out-of-line ctor.
- Every bridged signature referencing a J3D type needs SB_ENGINE_TYPE for it, or the pointer marshals
  as a guest address (fault). Build the type set per bridged-method closure.
- Heap: the port loader's aligned allocations use the current JKRHeap (sb_heap_bringup, idempotent,
  called in the load bridge). Plain `new` in the slice uses host malloc (SMSPORT_GUEST_SLICE). Both
  host-backed; no teardown/free in the slice yet (models long-lived).
- The link infra (133b247) is reusable: add new port/bridge sources to smsport_bridge; the start-group
  pulls the closure; gate the runtime override half behind the build option.
