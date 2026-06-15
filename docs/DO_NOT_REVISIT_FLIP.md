# ⛔ DO NOT REVISIT: the "flip" / host-layout-engine architecture (RETIRED 2026-06-15)

This is a permanent do-not-redo marker. A whole architectural direction was built over
~2026-06-14/15 and then **deliberately deleted** by user directive. Do not resurrect any
part of it. Read this before proposing anything that touches engine-object representation,
the recompiler's type system, or a `port/` host engine.

## What "the flip" was (the retired idea)
Convert engine objects (J3DModelData, J3DMaterial, JKRHeap, JUTTexture, …) from their
GameCube guest-RAM layout into **host-native C++ objects** living outside the guest arena,
and bridge recompiled gameplay to them. Its machinery:
- a **PC-native host-layout engine** in `port/` (the JSystem decomp compiled as host C++,
  8-byte pointers, host struct layout);
- an **engine-object HANDLE table** (`runtime/eng_handle`, 0x9xxxxxxx tokens) so guest code
  could hold host objects;
- a **tailored recompiler** that emitted **bridged getters** (`sbget_<T>_<field>`),
  **construction→handle** rewrites (`sbnew_<T>`), and **offset-0 virtual-dispatch routing**
  (`sbvirt_<T>_<slot>` / `virt_thunks`), driven by a seeded **type-recovery / type-DB**
  pipeline (`type_recovery`, `type_db_build`, `decomp_parse`, `func_sig`, `abi_layout`,
  `vtable_db`) and `SUNBRIGHT_ENGINE_TYPES`/`FIELD_TYPES`/`HOLDER_TYPES`/`OWN_TYPES`/
  `VIRT_TYPES`, gated behind `SB_FLIP_J3D`;
- BE→host asset swappers (`port/assets/bmd_swap`, `bmt_swap`, `anm_swap`) feeding the host
  loaders.

## Why it was abandoned (the root problem, named)
A flipped type changes its **storage model** (host layout + handle token, NOT a guest
pointer in the shared RAM arena). The recomp↔engine boundary then breaks the shared-memory
invariant: **every** function that derefs the type must be bridged or handle-aware, or it
loud-faults on the token. That makes a flip **all-or-nothing over the type's entire consumer
closure**, which for J3D spans ~46 game actors that **inline** engine field reads/array
indexing (`getMaterialNum`, `mMaterials[i]`, …) deep inside AI/behavior code we must not
port. The array-of-engine-pointers case (`J3DMaterial** mMaterials` indexed stride-4 by guest
code vs 8-byte host pointers) is one of many places where guest-layout assumptions are baked
into gameplay and cannot be cleanly bridged. The closure never closed.

## The architecture that REPLACED it (the one true path)
**Same GameCube memory layout, everywhere.** Engine objects stay guest-RAM, GC-layout
(32-bit big-endian pointers, GC offsets) in the shared 24 MB arena. **PC owns the engine
code as native C++ that operates ON that guest layout** (the way `native_jas`,
`sms_drawsync_lossproof`, `native_card`, and the native renderer in `runtime/render/` +
`runtime/ngx/` already do — reading J3D objects straight from guest RAM). **Gameplay stays
recompiled** and runs directly on the same memory, so `mMaterials[i]` is a plain guest load
that Just Works. The boundary is **plain function-call overrides over shared guest memory** —
no handles, no getters, no marshalling, no virtual-dispatch routing, because both sides see
identical bytes. Goal: no Dolphin (own GPU/renderer/OS/audio natively). The live frontier is
the native renderer (`docs/native_port_plan.md`, N5 per-material TEV combiner next).

## What was deleted (commit that removed it; recover from history if ever needed)
Removed wholesale: `port/` (host engine), `runtime/eng_handle.*`, `runtime/bridge.h`, the
recompiler type machinery (`type_recovery`, `type_db_build`, `type_db`, `decomp_parse`,
`func_sig`, `abi_layout`, `vtable_db`) + their tests, the j3d bridge overrides, the
`SB_FLIP_J3D` CMake wiring + object libs, `docs/ARCHITECTURE_TARGET.md` and the flip RE notes
(`object_identity`, `first_flip_endianness`, `j3dmodeldata_flip_closure`,
`j3d_subsystem_ownership_plan`, `abi_findings`). The emitter (`c_emitter`) and `main.cpp`
were reverted to the plain guest-layout recompile; `intrinsics.h` and `memory_bridge.cpp`
were restored byte-identical to their pre-flip state. The last pre-flip keeper commit was
`9b935c8` ("ngx N5: TLUT…", 2026-06-14); the full flip block lived in the contiguous range
after it. If any RE detail is ever wanted (e.g. the BE asset-swap layouts in `*_swap.cpp`,
or the J3D vtable-slot order), recover it from git history at that range — but do NOT relink
the architecture.

— User directive 2026-06-15: "first eradicate the flip architecture completely, delete
everything related to it, leave a note to never revisit it." Done.
