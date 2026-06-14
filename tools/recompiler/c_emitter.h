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
};

// Emits C source code from a decoded sequence of PPC instructions.
// Output is #include-able into generated/functions.cpp and compiled as C++.
//
// Each PPC function becomes:
//   extern "C" void func_XXXXXXXX(CPUState& cpu) { ... }
//
// Memory accesses go through MEM_R32/MEM_W32 macros from intrinsics.h.
// All arithmetic is 32-bit unsigned; cast to s32 when needed for signed ops.

struct EmitContext {
    u32 func_addr;
    std::vector<PPCInstr> instrs;
    std::unordered_set<u32> branch_targets;     // within-function jump labels
    std::unordered_set<u32> jumptable_targets;  // bctr jump-table case labels (subset of branch_targets)
    std::map<u32, EngField> eng_fields;         // load/store pc -> host engine-object field access (tailored)
};

class CEmitter {
public:
    explicit CEmitter(std::ostream& out) : out_(out) {}

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
    int unhandled_ = 0;
    std::vector<std::string> unhandled_ops_;

    void emit_instr(const PPCInstr& i, const EmitContext& ctx);

    // Tailored-boundary load/store: if `i` is registered in ctx.eng_fields, emit a
    // direct host-struct member access (instead of MEM_R*/MEM_W*) and return true.
    bool emit_eng_field(const PPCInstr& i, const EmitContext& ctx);

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
