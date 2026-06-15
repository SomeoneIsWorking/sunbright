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

## ✅ STEP 0 (DONE 2026-06-15, commit pending) — a tailored flip now COMPILES (Option A landed)
RESOLVED via Option A (bridge-call accessors). The recompiler no longer emits host-struct names into
generated game TUs; it emits CALLS to `extern "C"` thunks and writes their DEFINITIONS to
`generated/eng_accessors.cpp` (the one TU compiled WITH the decomp headers, ABI-correct by name).
PROVEN on REAL recompiler output (`SUNBRIGHT_ENGINE_TYPES=JUTTexture`): all 6 flipped game TUs
compile with NO decomp headers in scope (the u64 collision is structurally impossible), and
`eng_accessors.cpp` compiles with the decomp headers + port shims and NO cpu_state.h. Standalone
gate `runtime/tests/run_flip_compile_test.sh` compiles+links+RUNS a flipped function end-to-end.
What landed:
- Emitter (`c_emitter.{h,cpp}`): `emit_eng_field` → `sbf_<T>_<member>_<op>_<hash>(handle[,v])` calls;
  construction → `sbnew_<T>_<hash>()`; stack temp → `SbDynStackObj(sbsizeof_<T>_<hash>())`. An
  `EngAccessorTable` collects the thunk defs (deduped by symbol). NAME-based bodies (host compiler
  computes offsets) — NOT numeric offsets.
- Runtime: `runtime/eng_accessor_rt.h` (decls + `sb_set_guest_ptr` template, NO cpu_state.h) for the
  accessor TU; `SbDynStackObj` (size-at-runtime RAII) in `intrinsics.h` for generated TUs.
- main.cpp: writes `eng_accessors.h` (decls, #included by functions.h) + `eng_accessors.cpp`
  (#includes the flipped types' decomp headers via `type_db_build` `type_headers`).
- CMake: `generated/eng_accessors.cpp` built with the port shim/include env (harmless when empty).
- LANDMINE handled: the type DB's `__vtbl` SENTINEL (appended-vtable slot of a polymorphic subclass)
  is NOT a real member — the emitter SUPPRESSES that inlined guest vtable store (host construction
  owns the vtable). JUTTexture compiles BECAUSE of this. ⚠ Still OPEN (does not block compile, blocks
  runtime correctness of polymorphic-subclass flips): actually SETTING the host vtable for an inlined
  polymorphic ctor (object_identity.md option 2). Don't rely on a polymorphic-subclass type being
  runtime-correct until that ctor-bridge routing lands.

### (historical) the blocker STEP 0 fixed — generated engine-types code did NOT COMPILE
The de-risk validated the flip EMISSION against STUB struct definitions in a harness; the real
compile-into-the-binary path was never exercised, and it was BROKEN two layers deep. Verified by
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
1. **J3DModel construct + calc bridge (GX-free). — PORT + BRIDGE HALF DONE & VERIFIED (2026-06-15).**
   Built + committed:
   - **Port engine runs natively** (commit "STEP 1 foundation"): `model_calc_run` ctest loads a
     bmd_swap'd BMD → `new J3DModel(md,0,1)` → `J3DModel::calc()` → finite node matrices, 14/14 BMDs.
     calc IS GX-free as claimed. ONE engine global needed: `JMANewSinTable(0xC)` (Application.cpp:390)
     — calc's joint transform math (J3DGetTranslateRotateMtx → JMASSin/Cos) reads the global
     jmaSinTable, null until that init. Now in the idempotent `sb_j3d_bringup()`.
   - **Bridge layer** (commit "J3DModel construct+calc bridge layer"): `port/bridge/j3d_bridge.cpp`
     free-fns `sbport_j3dmodel_ctor` (PLACEMENT-NEW onto the sbnew raw buffer → host vtable+initialize+
     entryModelData), `_entryModelData`, `_calc`, `_viewCalc`, `_getNodeMtx`; `runtime/overrides/
     j3d_model_bridge.cpp` SUNBRIGHT_OVERRIDEs at ctor 0x802dde2c / entryModelData 0x802ddf90 / calc
     0x802debc4 / viewCalc 0x802deeb8 resolving handles→host. Gate renamed SB_FLIP_J3DMODELDATA →
     **SB_FLIP_J3D** (the slice is now load+construct+calc as one unit). Default build links clean.
   - **End-to-end verification** (commit "verify ... real handle table"): `j3d_bridge_run` ctest runs
     the EXACT override sequence through the real `runtime/eng_handle.cpp` (sb_eng_handle/host
     round-trips, `::operator new(sizeof(J3DModel))` == generated sbnew, placement-new ctor, calc,
     handle-resolved getNodeMtx) and asserts node matrices BIT-IDENTICAL to the direct path. 14/14 ok.
     Proves the runtime half is sound for construction + placement-new host vtable + calc.

   **⛔ DECISIVE BLOCKER for the game-driven verification: `calc__8J3DModel` 0x802debc4 has ZERO direct
   `bl` callers — it is ALWAYS dispatched VIRTUALLY** (`scratch/callers 0x802debc4` = empty). A virtual
   call loads the vtable from the J3DModel HANDLE token (`lwz r12,0(handle)`) and faults in the memory
   bridge before the bctrl. So the calc override never fires from real game code, and "drive the game
   to model load+calc, compare matrices" is **GATED on the offset-0 virtual-dispatch routing**
   (recompiler must recognize `lwz rX,0(engine-handle); …; bctrl` and route to the host vtable / by
   static type — docs/ARCHITECTURE_TARGET.md function-call half, listed as a separate item but Step 1's
   game verification needs it). The CTOR 0x802dde2c, by contrast, HAS many direct `bl` callers
   (TJointModel::initActor, TLensFlare, TSunModel, TEnemyManager, …) — construction IS wireable now.

   **(a) Engine-types recompile compile-check — DONE & GREEN (2026-06-15).** `SUNBRIGHT_ENGINE_TYPES=
   "J3DModel"` (J3DModel ONLY — see below) `SUNBRIGHT_DISCOVER_POINTERS=1 SUNBRIGHT_DISCOVER_CODEPTRS=1`
   recompile to `generated-flip/` (gitignored; reusable, no re-recompile needed next session). Result:
   ALL 55 generated game TUs compile with NO decomp headers (`-I runtime -I generated-flip`) and
   `eng_accessors.cpp` compiles in the port world (`-I port/compat/include -I reference/sms/include`,
   shims, NO cpu_state.h). Construction emits `sbnew_J3DModel` / `sbnew_SDLModel` (most-derived). 6
   accessor thunks. The full-scale LINK is mechanically proven by `runtime/tests/run_flip_compile_test.sh`
   (links+runs a flipped fn + accessor TU + boundary runtime) — only scale differs; next session's
   first action is the SB_FLIP_J3D binary build off `generated-flip/` (cheap, just heavy: swap
   generated/ → generated-flip, `cmake -B build -DSB_FLIP_J3D=ON`, build).
   TWO recompiler/scoping findings that made (a) compile:
   - **Engine-internal methods must NOT be field-flipped** (recompiler fix, committed): flipping a
     type also processed its OWN methods (viewCalc/calcNrmMtx) → sbf_ accessors for the type's
     internal graph-pointer fields (host 8-byte ptrs) → accessor TU didn't compile. main.cpp now
     excludes functions whose demangled class is a flipped type (they're bridged/dead) from the flip.
   - **Flip J3DModel ONLY, not J3DModelData.** Per Option 1 J3DModelData is held as an opaque HANDLE,
     never field-flipped in game code. Including it flipped a game free-fn `SMS_SettingDrawShape`
     (J3DModelData*) that reads `mVertexData.mVtxPosArray/Norm/TexCoord` (host-graph ptrs) inline →
     non-compiling accessors. That fn is DRAW code (bridged at Step 3); as an opaque handle it stays
     guest-typed in game code and loud-faults on the handle token if reached unbridged (fail-fast).
     (J3DModel's own remaining accessors mModelData/mMatPackets/mShapePackets/mVertexBuffer are
     pointer fields read by draw-path callers stampModel/entryStaticDrawBuffer* → emitted as
     guest_ptr translations that COMPILE; runtime-correct only once those draw callers are bridged.)
   (b) **NEXT: offset-0 virtual-dispatch routing** (needed for calc to fire from the game) — then the
       game-driven matrix comparison becomes reachable. Also: build the SB_FLIP_J3D binary off
       generated-flip/ and confirm it boots (link + no-regression with the flip + bridge overrides).
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
