#pragma once
#include "ppc_decoder.h"
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

// =============================================================================
// type_recovery — SEEDED type recovery (reg -> host-native engine type at each site).
//
// Computes, at each `lwz/lfs/stw/...` site, whether the base register points at a
// HOST-NATIVE engine object — and if so, which host member — SEEDED by the function's
// decomp signature (its `this`/param types are known) and propagated through the
// function's dataflow.
//
// STATUS (2026-06-15): the FIELD-FLIP emitter that consumed this pass's `eng_fields`
// output is RETIRED — the game<->engine boundary is FUNCTION-CALLS-ONLY
// (docs/ARCHITECTURE_TARGET.md; engine objects cross as handles, game code never
// field-derefs them). This pass + EngField SURVIVE as the reg->engine-type DATAFLOW
// that the function-call boundary still needs: offset-0 VIRTUAL-DISPATCH-on-handle
// routing (knowing `rA` at a `lwz r,0(rA); …; bctrl` is an engine handle of type T) and
// bridge-arg marshalling. The `eng_fields` map is now the analysis result, not emitter
// input; unit-tested standalone (type_recovery_test, coverage_real/sweep).
//
// Portable C++17 (no host-arch / endianness assumptions): pure analysis over decoded
// PPCInstr. Builds the same on x86-64 and arm64.
// =============================================================================

// The recovered type of a load/store site whose base register is statically a host-native
// PC-engine object: the host C++ type + member to access. (Historically consumed by the
// field-flip emitter, now RETIRED — see docs/ARCHITECTURE_TARGET.md; the boundary is
// function-calls-only. recover_eng_fields and this struct survive as the reg->engine-type
// DATAFLOW that virtual-dispatch-on-handle routing and bridge marshalling will reuse.)
struct EngField {
    std::string type_cname;       // host C++ type name of the BASE object, e.g. "EngineCam"
    std::string member;           // host member expression, e.g. "mFov"
    std::string ptr_type_cname;   // if this field is an engine-object POINTER, the type it points at; "" otherwise
    bool guest_ptr = false;       // true if this field is a POINTER to GUEST data (not an engine object)
};

// A recognized VIRTUAL CALL through an engine handle (offset-0 virtual-dispatch routing,
// docs/ARCHITECTURE_TARGET.md function-call boundary). At the `bctrl` whose CTR was loaded by
// the chain `lwz vt,0(rA); lwz m,N(vt); mtctr m` with rA statically an engine type, we record
// the base type and the guest vtable byte-offset N. The EMITTER validates N against the
// vtable-slot DB (vtable_db.h) for that type and, if it names a method, routes the call to the
// host virtual method instead of `call_ppc(cpu, cpu.ctr)` (which would fault on the handle's
// vtable load). An unvalidated record harmlessly falls back to call_ppc.
struct VCall {
    std::string type;        // engine type leaf of the dispatched-on base register
    int         vtbl_off;    // guest vtable byte-offset of the loaded method slot
    // PCs of the feeder instructions that load the method address out of the guest vtable
    // (`lwz vt,0(handle); lwz m,N(vt); mt(lr|ctr) m`). When the terminal branch is routed to a
    // host-dispatch thunk these are DEAD — and the `lwz vt,0(handle)` would FAULT on the engine
    // handle token (0x9xxxxxxx is unmapped guest memory). The emitter suppresses them.
    std::vector<u32> feeder_pcs;
};

// A field of a host-native engine type at a given guest displacement.
struct FieldDesc {
    std::string member;        // host member expression, e.g. "mFov"
    std::string nested_type;   // if this field is itself an engine-object POINTER, the
                               // engine type it points at (so loads THROUGH it stay
                               // typed — chained field access); "" for a scalar/value field.
    bool guest_ptr = false;    // true if this field is a POINTER to GUEST data (e.g. ResTIMG*,
                               // void* into a loaded archive) — NOT an engine object. On the host
                               // it is an 8-byte host pointer into guest RAM, but the recompiled
                               // game reads/writes a 32-bit guest ADDRESS, so the boundary must
                               // translate host<->guest (sb_host_to_guest / sb_guest_to_host)
                               // instead of truncating the host pointer to a scalar.
};

// Per-engine-type host layout, keyed by GUEST displacement (from decomp headers).
struct EngineLayout {
    std::map<int, FieldDesc> fields;
};

// Layout database + per-function signature seeds.
struct TypeDB {
    std::map<std::string, EngineLayout> layouts;                 // type name -> layout
    // func guest addr -> {GPR index (3..10, EABI arg slots) -> engine type name}.
    std::map<u32, std::map<int, std::string>> signatures;
    // Object-identity most-derived-type recognition (docs/re_notes/object_identity.md):
    //   sizes : type name -> guest object size in bytes (the value materialized by the `li`
    //           before `operator new`). 0/absent = unknown.
    //   bases : type name -> immediate base class name ("" = none). Lets a base type resolved
    //           from a called method's signature be UPGRADED to the most-derived subclass whose
    //           guest size matches the allocation `li` — so a polymorphic subclass construction
    //           (J2DWindow::Texture : JUTTexture) is recognized as the subclass, not the base,
    //           and its appended-vtable write stops being a recovery gap.
    std::map<std::string, int>         sizes;
    std::map<std::string, std::string> bases;
    // Return-type seeding (offset-0 virtual dispatch coverage): guest addr of a function whose
    // decomp RETURN type is a pointer/ref to an ACTIVE engine type -> that type leaf. After a `bl`
    // to such a target, r3 holds the returned engine object, so the lattice types it — letting a
    // `getModel()->virtual()` chain be recognized. (GNU-v2 mangling omits return types, so this
    // comes from the decomp headers, not the symbol.)
    std::map<u32, std::string>         return_types;
};

// Recover eng_fields (load/store pc -> host field access) for ONE function body.
// `instrs` is the collected (in-order) decode; `func_addr` selects the signature seed.
// `branch_targets` are the intra-function label pcs and `jumptable_targets` the computed-bctr
// case pcs (both from func_collect) — they define the control-flow graph this pass runs a
// forward dataflow fixpoint over, so a register's type at a join is the MEET of all incoming
// paths (a type kept only when every predecessor agrees) and loops converge. Passing empty
// sets degrades to a single straight-line pass.
// If `unmapped` is non-null, it is filled with the pcs of load/store sites whose base register
// IS an engine type but whose displacement is NOT in that type's layout — the dangerous
// "typed base, unmapped offset" misses (the SHARP EDGE): the emitter would fall back to a guest
// MEM access against a host handle = a correctness bug. A COMPLETE recovery leaves this empty.
//
// OBJECT-IDENTITY / back-typing (docs/re_notes/object_identity.md): a register used as `this`
// (arg0) of a known engine method/ctor IS that engine type, and that type propagates BACKWARD to
// the register's definition — which a purely forward pass cannot see. This is what types the
// INLINED ctor field writes that PRECEDE the revealing call (the handoff CRUX), and what flags the
// engine-CONSTRUCTION site. Opt in by passing the two extra params (existing callers pass nullptr
// and get the unchanged forward-only behavior):
//   * `raw_allocators` — guest addrs of raw allocators (`operator new` 0x802c3ba4, …). A
//     raw-allocator `bl` whose result is demanded as engine type T is a heap `new T` → flagged.
//   * `alloc_sites` — filled with {site pc -> engine type}: the heap `operator new` bl OR the
//     interior-stack `addi rD,r1,off` whose result becomes an engine object (the CONSTRUCTION
//     sites a function-call-boundary factory bridge will key on). When `alloc_sites` is non-null
//     the backward pass also runs, so pre-call inlined field writes get typed into `out`.
std::map<u32, EngField> recover_eng_fields(const std::vector<PPCInstr>& instrs,
                                           u32 func_addr, const TypeDB& db,
                                           const std::unordered_set<u32>& branch_targets,
                                           const std::unordered_set<u32>& jumptable_targets,
                                           std::vector<u32>* unmapped = nullptr,
                                           const std::unordered_set<u32>* raw_allocators = nullptr,
                                           std::map<u32, std::string>* alloc_sites = nullptr,
                                           std::map<u32, VCall>* vcalls = nullptr);
