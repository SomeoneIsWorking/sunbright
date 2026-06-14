#pragma once
#include "ppc_decoder.h"
#include "c_emitter.h"   // EngField
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

// =============================================================================
// type_recovery — SEEDED type recovery for the TAILORED-RECOMP boundary.
//
// The field-access half of the game<->engine boundary (docs/ARCHITECTURE_TARGET.md)
// needs to know, at each `lwz/lfs/stw/...` site, whether the base register points at a
// HOST-NATIVE engine object — and if so, which host member. This pass computes that
// (the `eng_fields` map the emitter consumes), SEEDED by the function's decomp
// signature (its `this`/param types are known) and propagated through the function's
// dataflow. The slice's hand stub (field_slice_gen.cpp seed_fields) is replaced by
// this real pass.
//
// SHARP EDGE (a de-risk #2 finding — recorded here so it isn't forgotten): in the
// guest-only recompiler an unknown register type is harmless (a generic big-endian MEM
// access against guest RAM is always correct). In the TAILORED build it is NOT: if this
// pass MISSES an engine-field access (leaves the site untyped), the emitter emits a
// guest MEM access against a 32-bit HANDLE — reading unrelated bytes = a correctness
// bug. So recovery must be COMPLETE for engine-typed sites, not merely sound. This pass
// is conservative about *introducing* a type (only via explicit, verified flow) but a
// miss is a latent bug, not a safe fallback. Coverage is the hard problem at scale.
//
// Portable C++17 (no host-arch / endianness assumptions): pure analysis over decoded
// PPCInstr. Builds the same on x86-64 and arm64.
// =============================================================================

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
//     interior-stack `addi rD,r1,off` whose result becomes an engine object. The emitter rewrites
//     a flagged heap site to `sb_eng_alloc()` (host storage + handle). When `alloc_sites` is
//     non-null the backward pass also runs, so pre-call inlined field writes get typed into `out`.
std::map<u32, EngField> recover_eng_fields(const std::vector<PPCInstr>& instrs,
                                           u32 func_addr, const TypeDB& db,
                                           const std::unordered_set<u32>& branch_targets,
                                           const std::unordered_set<u32>& jumptable_targets,
                                           std::vector<u32>* unmapped = nullptr,
                                           const std::unordered_set<u32>* raw_allocators = nullptr,
                                           std::map<u32, std::string>* alloc_sites = nullptr);
