#pragma once
#include "ppc_decoder.h"
#include <map>
#include <string>
#include <sstream>
#include <unordered_set>
#include <vector>

// Emits C source code from a decoded sequence of PPC instructions.
// Output is #include-able into generated/functions.cpp and compiled as C++.
//
// Each PPC function becomes:
//   extern "C" void func_XXXXXXXX(CPUState& cpu) { ... }
//
// Memory accesses go through MEM_R32/MEM_W32 macros from intrinsics.h.
// All arithmetic is 32-bit unsigned; cast to s32 when needed for signed ops.

// A virtual `bctrl` (offset-0 dispatch on an engine handle) routed to a HOST virtual method
// (docs/ARCHITECTURE_TARGET.md function-call boundary). main.cpp resolves recognition
// (type_recovery VCall) against the vtable-slot DB (vtable_db.h) into one of these per routed
// call site; the emitter replaces `call_ppc(cpu, cpu.ctr)` with a call to the generated thunk.
// Scope: zero-argument, void-returning virtuals (J3DModel calc/update/entry/viewCalc) — the
// thunk takes the handle from r3. Signatures with args fall back to call_ppc (not routed).
struct EmitVirtCall {
    std::string type;     // host engine type leaf, e.g. "J3DModel"
    std::string method;   // host method name, e.g. "calc"
    std::string thunk;    // generated thunk symbol to call, e.g. "sbvirt_J3DModel_2"
};

struct EmitContext {
    u32 func_addr;
    std::vector<PPCInstr> instrs;
    std::unordered_set<u32> branch_targets;     // within-function jump labels
    std::unordered_set<u32> jumptable_targets;  // bctr jump-table case labels (subset of branch_targets)
    std::map<u32, EmitVirtCall> virt_calls;     // bctrl pc -> routed host virtual call (empty = none)
};

// One generated virtual-dispatch thunk to define in the port-world thunk TU (decomp headers,
// no cpu_state.h): `extern "C" void <thunk>(u32 h){ ((<type>*)sb_eng_host(h))-><method>(); }`.
struct EmitVirtThunk {
    std::string thunk, type, method;
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

    // Virtual-dispatch thunks emitted across all functions (deduped) — main.cpp writes their
    // definitions into the port-world thunk TU.
    const std::vector<EmitVirtThunk>& virt_thunks() const { return virt_thunks_; }

private:
    std::ostream& out_;
    int unhandled_ = 0;
    std::vector<std::string> unhandled_ops_;
    std::vector<EmitVirtThunk> virt_thunks_;
    std::unordered_set<std::string> virt_thunk_seen_;

    void emit_instr(const PPCInstr& i, const EmitContext& ctx);

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
