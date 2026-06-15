#pragma once
#include "ppc_decoder.h"
#include <map>
#include <string>
#include <sstream>
#include <unordered_set>
#include <vector>

// TAILORED-RECOMP boundary, field-access half (see docs/ARCHITECTURE_TARGET.md).
// At a load/store site whose base register is statically typed as a *host-native*
// PC-engine object (type recovery seeded by decomp signatures), the emitter must
// NOT emit a raw guest-offset/big-endian MEM_R*/MEM_W* — it reads/writes the HOST
// struct member directly (host offset, host endianness, host pointer width). An
// EngField names the host C++ type + member to bake at that site; the guest
// displacement is subsumed by `member` (which already encodes the host offset).
// An empty eng_fields map = the legacy all-guest behaviour (no tailored sites).
struct EngField {
    std::string type_cname;       // host C++ type name of the BASE object, e.g. "EngineCam"
    std::string member;           // host member expression, e.g. "mFov"
    // If this field is itself an engine-object POINTER, the host C++ type it points at
    // (e.g. "EngineCam" for `EngineCam* mNext`); empty for a scalar/value field. An
    // engine-pointer field can't fit a raw host pointer in the 32-bit recomp register
    // file, so a load yields a HANDLE (sb_eng_handle) and a store consumes one
    // (sb_eng_host) — this is what makes chained field access (obj->next->field) work.
    std::string ptr_type_cname;
    // True if this field is a POINTER to GUEST data (not an engine object): on the host an
    // 8-byte host pointer into guest RAM, but the game reads/writes a 32-bit guest ADDRESS.
    // A load translates host->guest (sb_host_to_guest); a store guest->host (sb_set_guest_ptr).
    // Mutually exclusive with ptr_type_cname (engine-object pointer).
    bool guest_ptr = false;
};

// Emits C source code from a decoded sequence of PPC instructions.
// Output is #include-able into generated/functions.cpp and compiled as C++.
//
// Each PPC function becomes:
//   extern "C" void func_XXXXXXXX(CPUState& cpu) { ... }
//
// Memory accesses go through MEM_R32/MEM_W32 macros from intrinsics.h.
// All arithmetic is 32-bit unsigned; cast to s32 when needed for signed ops.

// One generated accessor thunk (docs/re_notes/j3d_subsystem_ownership_plan.md STEP 0, Option A).
// The generated functions_*.cpp CANNOT name host engine struct types (their decomp headers
// collide with runtime/cpu_state.h — the u64 typedef clash) and have no struct defs. So every
// host-type-dependent operation — a field load/store, an engine `operator new`, a stack-temp
// sizeof — is emitted as a CALL to one of these `extern "C"` thunks, whose DEFINITION is written
// to generated/eng_accessors.cpp and compiled with the decomp headers (the one TU that bakes the
// ABI-correct host offsets/sizes, by NAME, so the host compiler computes them). decl goes in
// generated/eng_accessors.h (included by functions.h); def goes in generated/eng_accessors.cpp.
struct EngAccessorDef {
    std::string decl;   // extern "C" <ret> <symbol>(<params>);
    std::string def;    // extern "C" <ret> <symbol>(<params>) { <body> }
};

// Accumulates the accessor thunks needed by an entire recompile run (deduped by symbol). One
// instance shared across all per-file CEmitters; main.cpp writes it out after emission. When a
// CEmitter has no table (unit tests), it still emits the deterministic accessor CALLS — it just
// doesn't record the defs (the test asserts the call form, not the thunk bodies).
struct EngAccessorTable {
    std::map<std::string, EngAccessorDef> by_symbol;
};

struct EmitContext {
    u32 func_addr;
    std::vector<PPCInstr> instrs;
    std::unordered_set<u32> branch_targets;     // within-function jump labels
    std::unordered_set<u32> jumptable_targets;  // bctr jump-table case labels (subset of branch_targets)
    std::map<u32, EngField> eng_fields;         // load/store pc -> host engine-object field access (tailored)
    // Engine-CONSTRUCTION sites (type_recovery find_alloc_sites; docs/re_notes/object_identity.md):
    // pc of a guest `operator new` bl -> host C++ type name to allocate. At such a site the emitter
    // emits `cpu.gpr[3] = sb_eng_alloc<Type>()` (raw host storage + a 32-bit handle) instead of the
    // guest call, so the recompiled (inlined/bridged) ctor initializes a HOST object. Empty = no
    // construction rewrites (the legacy path). (Interior-stack temps — Pattern C — are not yet here.)
    std::map<u32, std::string> alloc_sites;
};

class CEmitter {
public:
    // `accessors` (optional) collects the engine-accessor thunk defs this emitter generates. Pass
    // one shared table across all per-file CEmitters in a real recompile; pass nullptr in unit
    // tests that only assert the emitted CALL form.
    explicit CEmitter(std::ostream& out, EngAccessorTable* accessors = nullptr)
        : out_(out), accessors_(accessors) {}

    // Emit the file header (includes, macros)
    void emit_header();

    // Emit the jump table: address → function pointer
    void emit_jump_table(const std::vector<u32>& func_addrs);

    // Emit a single function
    void emit_function(const EmitContext& ctx);

    // Count of unhandled instructions encountered
    int unhandled_count() const { return unhandled_; }
    const std::vector<std::string>& unhandled_mnemonics() const { return unhandled_ops_; }

private:
    std::ostream& out_;
    EngAccessorTable* accessors_ = nullptr;
    int unhandled_ = 0;
    std::vector<std::string> unhandled_ops_;

    void emit_instr(const PPCInstr& i, const EmitContext& ctx);

    // Tailored-boundary load/store: if `i` is registered in ctx.eng_fields, emit a CALL to a
    // generated host-field accessor thunk (instead of MEM_R*/MEM_W*) and return true.
    bool emit_eng_field(const PPCInstr& i, const EmitContext& ctx);

    // Accessor-thunk symbol generators. Each returns the `extern "C"` symbol to CALL from the
    // generated code, and (when accessors_ is set) records the thunk's decl+def into the table.
    std::string eng_field_symbol(const EngField& f, PPCOp op);  // field get/set thunk
    std::string eng_new_symbol(const std::string& type);        // sbnew_<T>: host alloc -> handle
    std::string eng_sizeof_symbol(const std::string& type);     // sbsizeof_<T>: host sizeof(T)

    // Helpers
    std::string ea(const PPCInstr& i);   // effective address: rA+d or rA+rB
    std::string ea_x(const PPCInstr& i); // indexed: rA+rB
    std::string cr_bit(u8 bi);           // "cpu.cr[N].{lt|gt|eq|so}"
    void set_cr0(const PPCInstr& i, const std::string& result);
    void set_carry(const std::string& result, const std::string& a, const std::string& b);

    void line(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
};

// Build the list of functions from an entry point + linear scan.
// Returns function start addresses.
std::vector<u32> find_functions(const u8* text, u32 base_addr, u32 size);
