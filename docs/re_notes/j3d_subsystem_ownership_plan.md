# Owning J3D as a port/ subsystem — execution plan (2026-06-15, user-chosen Option 1)

## ✅ J3D ANIMATION LOADER bridged (2026-06-15) — step 1 of the TShimmer animation consumer closure
After the .bmt material-table path landed, the live fault was the J3D animation consumer closure in
`load__8TShimmerFR20JSUMemoryInputStream` (0x8019f5ac). Step 1 (the animation-file loader) is DONE:
- **anm_swap** (`port/assets/anm_swap.{h,cpp}`): BE→host swapper for the WHOLE J3D1 animation block
  family — KEY: ANK1/TTK1/PAK1/CLK1/TRK1/VCK1; FULL: ANF1/PAF1/TPT1/CLF1/VAF1/VCF1. Element widths
  mirror J3DAnmLoader.cpp's readAnm* readers (keyframe tables = u16 runs via J3DAnmKeyTableBase=3×u16;
  scale/trans/weight/SRTCenter = f32; rotation + KEY-family color/tevreg = s16; FULL-family color +
  updateTexMtxID = u8/no-swap; J3DAnmTexPatternFullTable = mixed stride-8; C/KRegKeyTable = stride-0x1C;
  name tables = the shared `swap_ResNTAB_block` now exposed from bmd_blocks.h). It CLAMPS a block whose
  declared mSize overruns the buffer — the J3D file-header mFileSize understates real size by trailing
  block-alignment padding, and the in-game loader bounds nothing by it, so only padding is dropped, never
  a referenced region; never OOB.
- **Bridge** `J3DAnmLoaderDataBase::load` 0x802e8ca4 (static, `load__20J3DAnmLoaderDataBaseFPCv`): port
  `sbport_j3d_anm_load` (anm_swap → port `J3DAnmLoaderDataBase::load` → host J3DAnmBase) + runtime
  override `ov_j3d_anm_load` (SB_FLIP_J3D), returning a handle. Mirrors the .bmd/.bmt loader bridges.
- Tests: `anm_swap_test` (synthetic ANK1 + the mixed TPT1 table + uncovered-block refusal contract) and
  `anm_load_run` (gate: 288 real .bck/.btk/.btp/.brk from airport0.szs → non-null animators, sane
  getFrameMax). Both ctest; SKIP without `$SUNBRIGHT_ANM_DIR`/scratch/bmt/anm (scan_anm --write).
- VERIFIED: build-j3dvirt boots PAST the recompiled anm load — fault moved forward within TShimmer::load
  to pc=8019f658 (a 16-bit handle+0x16 deref). NEXT (step 2): searchUpdateMaterialID(md) 0x802e3dd4, then
  J3DMaterialAnm construction, J3DMaterial change/setMaterialAnm/setSomeFlag, entryTexMtxAnimator
  0x802dd448 + getFrameMax. The 16-bit handle+0x16 read at 8019f658 is a J3DAnmTextureSRTKey field read
  inline in TShimmer::load — disasm to choose FIELD_TYPES getter-route (+= J3DAnmTextureSRTKey) vs bridge.

## 🔎 RUNTIME EVIDENCE (2026-06-15, post bridged-getter emission 27ceadd) — the .bmt MATERIAL-TABLE path is the live fault
With `SUNBRIGHT_FIELD_TYPES="J3DModelData,J3DMaterial"` the inlined J3DModelData reads route to host
getters and the binary boots PAST createModelData/TShimmer::load. The NEXT fault (write-trap) is in
`J3DMaterialFactory::create` (802e55a0) writing a **GUEST-RAM** J3DMaterial (this=0x810b7ecc). Traced
the path: it is reached from `J3DModelLoaderDataBase::loadMaterialTable` (0x802e7128) → readMaterialTable
→ create. That is the **standalone .bmt material-table loader** (loads a `bmt2`/`bmt3` file → a
J3DMaterialTable), DISTINCT from the .bmd model `load` (0x802e6f00) which IS bridged. It is called from
MANY managers/actors (TMapObjManager, TElecNokonokoManager, TYumboManager, enemy managers, …) — a
WIDE, real path. So J3DMaterial is NOT exclusively host+handle today: the .bmt loader builds GUEST
J3DMaterials in guest RAM, and the getters (which expect a 0x9 handle) can't serve those.

**This is a self-contained ownership sub-unit (do it BEFORE / alongside the keeper closure below):**
1. ✅ **DONE — `bmt_swap`** (`port/assets/bmt_swap.{h,cpp}`). BE→host swapper for .bmt files. ALL real
   SMS .bmt are `bmt3` with exactly {MAT3, TEX1} (verified: sky/kibako/nozzleitem.bmt from airport0.szs
   via `scratch/bmt/scan_bmt`). Reuses bmd_swap's MAT3/TEX1 block swappers via `port/assets/bmd_blocks.h`
   (detail::swap_MAT3_block / swap_TEX1_block forwarders — no duplication). bmt2(MAT2)/MDL3 left
   uncovered (loud via all_covered==false; none seen). Tests: synthetic `bmt_swap_test` (ctest) + real
   `bmt_load_run` gate (3 real .bmt → non-null J3DMaterialTable, materials 4/13/2, non-null texture).
2. ✅ **DONE — bridge `loadMaterialTable` 0x802e7128.** Confirmed static-like by disasm (r3=data,
   returns table*). port `sbport_j3d_loadMaterialTable` (j3d_bridge.cpp: bmt_swap → port
   loadMaterialTable → host J3DMaterialTable) + runtime override `ov_j3d_load_material_table`
   (j3d_loader_bridge.cpp, SB_FLIP_J3D) → `sb_eng_handle`.
3. ⏩ **LIVE — J3DMaterialTable + J3DMaterial host+handle consumers.** Next runtime fault (below).
   `sbport_j3dmaterialtable_getMaterialNum` added; still need: getMaterial, `J3DModelData::set-
   MaterialTable` 0x802dd2dc, the material animators (TexMtxAnimator/TShimmer), and the engine
   functions called with a J3DModelData/J3DMaterial handle (searchUpdateMaterialID — see below).
4. ✅ **DONE — gate harness `bmt_load_run`** (mirrors bmd_load_run; SKIPs w/o *.bmt in scratch/bmt).
NOTE: the engine-internal exclusion already drops J3DMaterial's OWN methods from field-routing; the
trap fired in J3DMaterialFactory (a different class) — once loadMaterialTable is port-owned, the guest
factory + create never run.

### ✅ VERIFIED (2026-06-15, .bmt path landed) — boots PAST the J3DMaterialFactory write-trap
Built build-j3dvirt (SB_FLIP_J3D, generated-virt with FIELD_TYPES) with the .bmt bridge. The prior
fault `FATAL: inline engine-field WRITE has no setter: J3DMaterial::mColorBlock (handle 0x810b7ecc)` —
the guest J3DMaterialFactory::create building a GUEST-RAM J3DMaterial — is GONE (the bmt loader is now
port-owned; the guest factory never runs). NEW fault (scratch/logs/run-bmt1.log): `FATAL wild guest
read ea=0x0` in **`TShimmer::load` 8019f5ac** (pc 8019f62c), inside the engine fn
**`searchUpdateMaterialID__19J3DAnmTextureSRTKey(J3DModelData*)` 802e3dd4** (lr=802e3e20). TShimmer::load
loads a BMD (bridged), `new J3DModel` (OWN_TYPES), then `bl searchUpdateMaterialID(md_handle)` — an
ENGINE fn (excluded from field-flip) that field-derefs the J3DModelData handle (`lwz r27,0xB4(md)` =
mMaterialName, then JUTNameTab::getName/getIndex). It must be BRIDGED (override → port-native on
sb_eng_host(handle)), like SMS_ChangeTextureAll. THIS is the next consumer-closure seam (the run→find-
deref→bridge loop). r31=0x9000003e at fault confirms it's a handle deref.

## ⏩⏩ LIVE UNIT (2026-06-15) — NEXT #2: MODEL-INSTANCE + KEEPER subsystem ownership (the J3DModelData consumer closure)
**Closure breadth PLANNED here FIRST (per handoff). Key reframing finding: the entire consumer
closure is ALREADY COMPILED in `port/` (`port/core_sources.txt` lists SDLModel.cpp, ObjModel.cpp,
MActor.cpp, MActorData.cpp, conductor.cpp, SampleCtrlModel.cpp, M3UModel.cpp + the whole J3D graph).
So this is NOT a porting job — it is BRIDGE WIRING + CONSTRUCTION OWNERSHIP.** The fault is purely
that the recompiled GAME still runs these consumers on a J3DModelData *handle* it field-derefs.

### Why the boundary must sit ABOVE the M3DUtil model layer
`SDLModel::entryModelDataSDL` reads ~30 J3DModelData fields inline (getJointNum/getShapeNum/
getMaterialNodePointer/getDrawMtxData/getTexture[+0xAC]/getVertexData…); `SDLModelData::entrySameMat`
reads `unk0->getTexture()` (+0xAC — the live fault). These CANNOT stay recompiled under SB_FLIP_J3D
(they deref the 0x9xxxxxxx handle token). They must run PORT-NATIVE on the host object. Since
SDLModel IS-A J3DModel and MActor owns a SDLModel, the whole model-instance layer is one irreducible
unit. Boundary = the public API of the keepers + MActor; the game holds HANDLES.

### OWNED types (host+handle; all sources already in port/ core_sources.txt)
- J3D (done): J3DModelData✓, J3DModel✓ (+ SDLModel subclass).
- M3DUtil: **SDLModelData, SDLModel, MActor, MActorAnmData (MActorData.cpp), M3UModel, SampleCtrl{Model,ModelData,Node}.**
- Strategic: **TModelDataKeeper, TModelDataNode, TMActorKeeper.**
- conductor's `TConductor::registerSDLModelData` (80035148) just inserts into a TList — TConductor is
  game-side; the SDLModelData ctor calls `gpConductor->registerSDLModelData(this)`. Either own the
  insert (port-native TList on a host gpConductor mirror) OR override registerSDLModelData to a no-op
  for the slice (the list is only walked by entrySDLModels/draw, gated at Step 3 / GX). Start: no-op.

### Bridge surface (game → port). Each = override at guest addr → port thunk on `sb_eng_host(handle)`.
Addresses from sms_gmse01_funcs.txt (⚠ labels are shifted by one slot around 8023exxx — VERIFY each
by disassembly before wiring; e.g. real SDLModelData ctor = 8023e034, real entrySDLModels = 8023e098).
- **TModelDataKeeper** (Strategic/ObjModel.cpp): ctor `__ct__…FPCc` 8021d2e4, createAndKeepData
  8021d4c0, loadModelData(static) 8021d5a4, getNthData 8021d448, getIndex 8021d32c, getDataByName
  8021d448?/verify, getModelDataNum 8021d300, registerDataAndJoinNewNode 8021d61c (TModelDataNode).
- **TMActorKeeper** (ObjModel.cpp): ctors 0x… (2 forms, take TLiveManager*), createMActor,
  createMActorFromNthData, createMActorFromDefaultBmd, createMActorFromAllBmd, createAndRegister
  8021d228, getMActor(name) 8021d12c-ish, getMActor(int) inline.
- **MActor** (M3DUtil/MActor.cpp): big API — setModel, setMActorAnmData, calc, calcAnm, viewCalc,
  entry, update, frameUpdate, perform, setAnimation, setBck/Btk/Bpk/Brk/Blk/Btp(+FromIndex),
  checkCurAnm, getFrameCtrl, … Bridge INCREMENTALLY as the game exercises each (Step 3+).
- **SDLModel / SDLModelData**: mostly INTERNAL (constructed + used inside the above). SDLModel's
  virtuals (viewCalcSimple override; calc/entry/viewCalc inherited from J3DModel) → VIRT routing.

### THE CRUX (boundary rule, do not violate): port thunks must NEVER deref a GAME object
Some bridged signatures take game-side pointers the port must not field-deref:
- TMActorKeeper ctor takes `TLiveManager*` → calls `getModelDataKeeper()` (TObjManager+0x24) +
  `getMActorAnmData()` (TObjManager+0x20). BOTH fields hold OWNED handles (set when the game's
  `new TModelDataKeeper`/MActorAnmData ran). The runtime OVERRIDE (which CAN read guest RAM via the
  memory bridge) reads those two guest words, resolves them with sb_eng_host, and passes the host
  pointers to the port ctor — NOT the raw TLiveManager*.
- MActor::perform takes `JDrama::TGraphics*` (engine → owned handle); setLightData takes
  `TBGCheckData*` (map collision = guest data → marshal the needed Vec/values, don't pass the ptr).
Rule: at every bridge, classify each arg = {owned handle | guest-data ptr | scalar}; the override
marshals accordingly. This is the per-method integration work; reads are shallow (one field each).

### Mechanisms reused (all already built — see below): OWN_TYPES construction, method overrides,
SB_ENGINE_TYPE handle marshalling, VIRT_TYPES offset-0 virtual-dispatch routing. `new TMActorKeeper`
appears at ~hundreds of enemy sites — OWN_TYPES handles ALL of them generically (per-site count
irrelevant); one ctor override per type. vtable_db must learn SDLModel (subclass appended vtable).

### Execution slices (each commit+push; verify before next)
1. **Model-data keeping path (the LIVE fault, self-contained — scalar/handle boundary).**
   OWN_TYPES += TModelDataKeeper, TModelDataNode, SDLModelData. SB_ENGINE_TYPE each. Port bridge
   thunks + runtime overrides for: TModelDataKeeper ctor / createAndKeepData / loadModelData /
   getNthData / getIndex / getDataByName / getModelDataNum / registerDataAndJoinNewNode; SDLModelData
   ctor (stores unk0=J3DModelData handle→host, registerSDLModelData→no-op for now). Inputs are all
   name strings (guest-data ptr) + flags (scalar) + handles — NO game-object deref needed.
   VERIFY: SB_FLIP_J3D + OWN_TYPES binary boots PAST the createModelData/createAndKeepData fault
   (8021d524) without faulting on a J3DModelData/SDLModelData/keeper handle deref. Milestone = the
   next fault is downstream (TMActorKeeper/SDLModel), proving slice 1 closed its sub-closure.
2. **MActor keeper + model-instance construction.** OWN_TYPES += TMActorKeeper, MActor, SDLModel,
   MActorAnmData. ctor overrides (TMActorKeeper ctor marshals keeper+anmdata handles per the CRUX;
   createAndRegister → port new SDLModel + new MActor; SDLModel::entryModelDataSDL now runs PORT-NATIVE
   on the host J3DModelData — the big field-deref is sound). VERIFY: a host SDLModel is constructed
   from a host J3DModelData with finite node/draw-mtx buffers (port unit test like model_calc_run but
   via SDLModel(SDLModelData,flags,1)).
3. **MActor per-frame + virtual calc routing → ORACLE COMPARE (NEXT #3).** Bridge MActor::calc/
   calcAnm/viewCalc/entry/update; VIRT_TYPES += SDLModel so `model->calc()` routes to the host vtable
   (J3DModel::calc slot, inherited). Drive headless to a frame, oracle-compare SDLModel(J3DModel) node
   matrices vs DISABLE_RECOMP (j3d_bridge_run proves the bridge math; this proves the GAME reaches it).

### ⛔⛔ DECISIVE FINDING (2026-06-15, slice 1 in progress) — the consumer closure includes PERVASIVE INLINED engine-field reads in GAME code; function-call bridging alone is INSUFFICIENT
Slice 1 step 1 (bridge SMS_ChangeTextureAll @0x80236c3c) LANDED + VERIFIED: the SB_FLIP_J3D binary
boots PAST the createModelData fault (8021d524, ea=0x900000cf) to the NEXT consumer. Good — clean
function-call bridge, no recompiler change. But the next fault exposes the structural wall:

**`TShimmer::load` (8019f5ac, a GAME actor load) faults at ea=0x900000ee = J3DModelData handle +0xB4.**
Disassembly (scratch/dis 0x8019f5ac) shows it contains BOTH shapes:
- **Callable seams (bridgeable, like SMS_ChangeTextureAll):** `bl searchUpdateMaterialID` (802e3dd4,
  the +0xB4/getMaterialName read), `bl entryTexMtxAnimator` (802dd448). These I CAN override.
- **INLINED J3DModelData graph reads (the hard core, NOT bridgeable):**
  `lwz r3,0x44(this)`(=unk44 J3DModelData handle); `lhz r4,0x24(r3)` = **getMaterialNum** (+0x24
  scalar) and `lwz r,0x28(r3); lwzx` = **getMaterialNodePointer** (+0x28 mMaterials, a HOST J3DMaterial**
  → element is a host J3DMaterial* the game then calls change()/setMaterialAnm()/setSomeFlag() on).
  These are inlined into the GAME function — there is NO call seam to override. On a 0x9xxxxxxx handle
  they wild-fault.

**SCALE: ~46 GAME source files** (Enemy/Map/Player/Camera/NPC — `grep getModelData()->|->getMaterialNum()|
->getMaterialNodePointer|->getShapeNum()|->getJointNum()` minus J3D/M3DUtil) inline-read J3DModelData/
J3DMaterial/J3DShape accessors. This is PERVASIVE core gameplay, not isolated. So:

**The "gameplay never field-derefs engine objects" premise (no-field-flip correction, 2026-06-15) is
empirically FALSE for SMS** — gameplay pervasively inlines engine accessor reads. The closure cannot
be bridged purely as function calls. THREE ways forward (ARCHITECTURE FORK — user decision needed):
- **A. Recompiler emits BRIDGED GETTERS for inline engine-field reads.** type_recovery already types
  the base (it knows unk44=J3DModelData handle at the read site); emit `sbget_J3DModelData_materialNum(h)`
  /`sbget_..._materialNodePtr(h,i)→handle` CALLS instead of `MEM_R*(handle+off)`. This IS a function
  call (no host layout in the game TU), scales to all 46 files automatically. ⚠ BUT this is essentially
  the eng_accessors getter mechanism the user RETIRED (41aaa69). Reintroducing it (getters-only,
  graph-ptr→handle) needs user sign-off — it's a narrower, function-call-shaped subset of the retired
  field-flip, not the full host-struct-in-game-TU flip.
- **B. OWN every field-dereffing actor function in port/** (TShimmer::load + the ~46 files' load/setup/
  draw fns). Strictly honors no-field-flip (only port code field-derefs). Cost: hand-port a large,
  repetitive surface of model-setup glue; tension with "don't hand-port game behavior" (load/setup is
  glue-ish, borderline). Doesn't scale cheaply.
- **C. Reconsider the tailored-recomp-into-port direction** given pervasive inlined engine access
  (the "unicorn" question the user already answered NO to once — but this is new, quantified evidence).

**✅ RESOLVED (user, 2026-06-15): Option A — the recompiler handles the actor↔model relationship.**
"Owning actors is fine if we don't have to port every single thing like enemy AI, otherwise the
recompiler should handle the actor-model relationship." The inline reads are pervasively embedded in
AI/behavior fns (perform/movement/think/calcAnim) we must NOT port → recompiler emits BRIDGED GETTERS.
See memory `actor-model-relationship-recompiler`. Owning stays for pure-engine subsystems; whole-fn
consumer overrides (SMS_ChangeTextureAll) stay where the consumer is a callable seam.

#### Implementation plan — bridged-getter emission ✅ DONE (2026-06-15, commit 27ceadd)
The 5-step plan below is IMPLEMENTED, unit-tested (recomp_test 75/0), and verified: a
`SUNBRIGHT_FIELD_TYPES="J3DModelData,J3DMaterial"` regen boots build-j3dvirt PAST the prior
createModelData/TShimmer::load fault. Gated behind `SUNBRIGHT_FIELD_TYPES` (empty default =
byte-unchanged). Writes route to a loud `sb_eng_field_write_trap` (no setter yet, fail-loud).
Engine-INTERNAL methods of a field type are excluded from routing (fe1ba78 rule). **DECISIVE
follow-on finding:** the next fault is `J3DMaterialFactory::create` (802e55a0) building a GUEST-RAM
J3DMaterial — J3DMaterial is not exclusively host+handle, so its getters are only runtime-sound once
J3DMaterial CONSTRUCTION is port-owned. That is NEXT #2 (own the model-data/material loading path),
the consumer-closure unit this getter emission unblocks. (J3DModelData alone is host+handle → its
scalar getters are sound now.)

Resurrect the GETTERS-ONLY half of the eng_accessors emission removed in 41aaa69 (recover from
`git show 41aaa69~1:tools/recompiler/c_emitter.cpp` / `:main.cpp` / `:CMakeLists.txt`). The ANALYSIS
half SURVIVED (type_recovery EngField, type_db_build, decomp_parse, func_sig). Scope it to READS only;
do NOT reintroduce the host-struct-in-game-TU flip (no `((T*)host)->member` in generated code).
1. **Emitter** (c_emitter): at an inline engine-field READ where recover_eng_fields typed the base,
   emit `sbget_<T>_<member>(handle)` instead of `MEM_R*(handle+off)`. Out-kind by field type:
   scalar→value; engine-obj ptr (and `mArr[i]`)→`sb_eng_handle(host->member)` so the game holds a
   handle; guest-data ptr→`sb_host_to_guest(host->member)`. Collect {T,member,kind} into an accessor
   manifest (dedup by symbol). Keep WRITES to engine fields out of scope for now (game rarely writes
   engine fields inline; if hit, add sbset_ later) — fail-LOUD if a needed setter is missing, don't
   silently MEM_W.
2. **Accessor TU** (main.cpp): write `generated/eng_accessors.{h,cpp}`; the .cpp #includes the typed
   decomp headers (type_db_build type_headers) + port compat shims, NO cpu_state.h, and defines each
   `sbget_<T>_<member>` by NAME (host compiler computes the offset — ABI-correct; never emit numeric
   host offsets). functions.h #includes eng_accessors.h (extern "C" decls). This is the proven
   port-world-compile pattern (mirror sb_virt_thunks object lib, gate SB_FLIP_J3D).
3. **CMake**: build eng_accessors.cpp as its own object lib like sb_virt_thunks (decomp headers + port
   shims, SHELL: force-includes), gated SB_FLIP_J3D; link into sunbright.
4. **Gate it** behind the same env that drives recognition (SUNBRIGHT_VIRT_TYPES/a new SUNBRIGHT_
   FIELD_TYPES) so the default recompile is byte-unchanged. Add recomp_test cases (scalar/engine-ptr/
   array/guest-ptr getter emission). Then regenerate generated-virt with J3DModelData (+J3DMaterial/
   J3DShape as the closure needs) typed, rebuild build-j3dvirt, re-run headless: TShimmer::load's
   inlined getMaterialNum/getMaterialNodePointer now route to the host object → boots past it.
5. **Closure types**: J3DModelData reads need J3DMaterial/J3DShape handles to flow (getMaterialNode
   Pointer→J3DMaterial handle; the game then calls J3DMaterial::change/setMaterialAnm via direct bl →
   add those as whole-fn overrides or they're already engine methods). Add SB_ENGINE_TYPE + the type
   set incrementally as each new wild-deref surfaces (faithful debug-path loop: run→find deref→type
   the field/bridge the seam→re-run).

Until this lands, slice 1 is BLOCKED at TShimmer::load (8019f5ac, inlined +0x24/+0x28). The
SMS_ChangeTextureAll bridge is kept (real progress: boots past 8021d524).

### Slice 1 step 1 landed (SMS_ChangeTextureAll). Below: the prior virtual-dispatch / construction history.

## UPDATE 2026-06-15 (late) — SB_FLIP_J3D build integration DONE; routed-calc gated on 2 mechanisms
Offset-0 virtual-dispatch routing is now BUILT, WIRED, and CORRECT end-to-end (recompiler side):
- **Build integration:** `generated*/virt_thunks.cpp` compiles as its own PORT-WORLD object lib
  `sb_virt_thunks` (gated `SB_FLIP_J3D`); `-DSUNBRIGHT_GENERATED_DIR=generated-virt` selects the routed
  recompile. The SB_FLIP_J3D binary LINKS clean. Thunk `#include` is relative to reference/sms/include.
- **Feeder-load suppression (recompiler correctness fix):** the routed `bclrl` is the host thunk call,
  and the dead feeder chain `lwz vt,0(handle); lwz m,N(vt); mtlr m` is now ELIDED (it would otherwise
  FAULT — `lwz vt,0(handle)` reads the 0x9xxxxxxx token). VCall/EmitVirtCall carry `feeder_pcs`;
  emit_function skips them. Verified in regenerated generated-virt; recomp_test 57/0; e2e PASS.

**Game-driven routed-calc oracle compare is BLOCKED on consumer-closure bridging (root-caused, not a bug).**
1. **Construction→handle bridging — DONE (2026-06-15, commit pending).** Flip-free: a new
   `SUNBRIGHT_OWN_TYPES` env (subset of VIRT_TYPES that gets host+handle construction; M3UModel stays
   guest) drives the emitter to rewrite owned-type heap `operator new` sites (resolved via the kept
   `find_alloc_sites`, raw_allocator 0x802c3ba4) into `cpu.gpr[3] = sbnew_<T>()` — a port-world thunk
   `sb_eng_handle(::operator new(sizeof(T)))`. The guest null-check + holder store + `bl __ct__` run on
   the HANDLE; ov_j3dmodel_ctor placement-news onto sb_eng_host(handle). Verified: recomp_test 62/0;
   `SUNBRIGHT_OWN_TYPES=J3DModel` regen routes 35 real `new J3DModel` sites + emits sbnew_J3DModel; the
   ctor→handle→calc runtime chain is already bit-identical (j3d_bridge_run). Only HEAP operator-new `bl`
   sites are routed (stack-temp origins keep guest layout). ov_j3dmodel_ctor's stale comment corrected.
2. **Consumer-closure bridging — THE remaining blocker (large subsystem unit).** First headless fault
   under `SB_FLIP_J3D + OWN_TYPES=J3DModel`: PC **8021d524 in `createAndKeepData__16TModelDataKeeper`**
   reads a **J3DModelData handle + 0xAC** (r3=r28=0x90000023, ea=0x900000cf). The handle comes from the
   load bridge via `TModelDataKeeper::loadModelData` (ObjModel.cpp:37 — `J3DModelLoaderDataBase::load(res,
   flags)` -> handle, then `new SDLModelData(data)`); game code then field-derefs it. This is the known
   J3DModelData-consumer-closure problem (`j3dmodeldata_flip_closure.md`: ~26 fns traverse the
   J3DModelData object graph) — the field-flip is the WRONG tool; per Option 1 these consumers must be
   OWNED in port/ or bridged. Concrete first targets: TModelDataKeeper (createAndKeepData/loadModelData/
   getNthData/getIndex — ObjModel.cpp), SDLModelData (M3DUtil/SDLModel.cpp — thin J3DModelData wrapper,
   ctor stores unk0=model + gpConductor->registerSDLModelData), registerDataAndJoinNewNode, and whatever
   reads J3DModelData+0xAC. Scope it as "own the model-data loading/keeping path in port/", not one fn.
   This is the next session's unit (plan the closure breadth before coding).

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
   (b) **Offset-0 virtual-dispatch routing — MECHANISM DONE 2026-06-15** (commits df14c44, ad1aa3d,
       efe19c1, e174d2a, 1ac4b50). The recompiler can now route `model->calc()`/`viewCalc()`/etc. to a
       host-dispatch thunk, gated behind `SUNBRIGHT_VIRT_TYPES` (inert by default). Four pieces, all
       unit + e2e tested: vtable-slot DB (`tools/recompiler/vtable_db.{h,cpp}`, DOL-anchored — reads the
       real guest vtable; `vtable_db_test`), recognition (`recover_eng_fields` `vcalls` out-param),
       emission + port-world thunk-gen (`c_emitter` virt_calls/virt_thunks(); main.cpp writes
       `generated/virt_thunks.{h,cpp}`), and end-to-end dispatch (`runtime/tests/run_virt_dispatch_test.sh`).
       Scope: zero-arg void virtuals (calc/update/entry/viewCalc — `decomp_parse::simple_virtuals`).
       **FIRST REAL ROUTED CALLS landed (commit 3de4c6e).** Two findings made it work:
       (i) **the real CodeWarrior virtual-call form is `mtlr m; bclrl`, NOT `mtctr; bctrl`** (verified on
       M3UModel::perform 0x80237930) — recognition + emission now handle both; this was why 0 routed at
       first. (ii) **coverage needs MEMBER-FIELD typing of the holder**: J3DModel's getModel accessors are
       INLINED (only 1 symbol returns J3DModel*), so signature/return seeding alone find no base; activating
       the HOLDER (`SUNBRIGHT_VIRT_TYPES="M3UModel,J3DModel"`) types `this->unk8`=J3DModel* so `unk8->calc()`
       recognizes — 29 routed sites (25 calc/3 entry/1 viewCalc), virt_thunks.cpp defines
       sbvirt_J3DModel_{1,2,3}=entry/calc/viewCalc (slot indices match the vtable DB). Return-type seeding
       (`TypeDB::return_types`) was also built (correct + general, for non-inlined returns).
       **NEXT (the runtime/build integration):** CMake-wire `generated/virt_thunks.cpp` under SB_FLIP_J3D
       (port-world compile — decomp headers + port/compat shims; mirror `port/bridge/j3d_bridge.cpp`; this is
       the removed eng_accessors object-lib pattern, recoverable from git pre-41aaa69; sb_eng_host already
       linked), build the SB_FLIP_J3D binary off generated-virt/, drive headless to a calc, compare host
       J3DModel node matrices to the DISABLE_RECOMP oracle (j3d_bridge_run proves the bridge; this proves the
       game reaches it). Also bridge the J3D consumers on the path so the game reaches a calc without
       faulting on an unbridged J3DModelData/J3DModel deref (overlaps GX/draw ownership).
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
