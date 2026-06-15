#pragma once
#include "ppc_decoder.h"
#include "type_recovery.h"   // EngField (bridged-getter emission for inline engine-field reads)
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
    // PCs of the guest vtable/slot loads + mt(lr|ctr) feeding this call. They are DEAD once routed
    // to the thunk, and the `lwz vt,0(handle)` would fault on the handle token — emit_function
    // suppresses them.
    std::vector<u32> feeder_pcs;
};

struct EmitContext {
    u32 func_addr;
    std::vector<PPCInstr> instrs;
    std::unordered_set<u32> branch_targets;     // within-function jump labels
    std::unordered_set<u32> jumptable_targets;  // bctr jump-table case labels (subset of branch_targets)
    std::map<u32, EmitVirtCall> virt_calls;     // bctrl pc -> routed host virtual call (empty = none)
    // pc of an owned-engine-type heap `operator new` bl -> the type to construct as a host object +
    // handle. The emitter replaces the guest alloc with `cpu.gpr[3] = sbnew_<T>()`; the out-of-line
    // ctor override then placement-news the host object onto sb_eng_host(handle). Empty = none.
    std::map<u32, std::string> alloc_sites;
    // INLINE engine-field reads (docs/re_notes/j3d_subsystem_ownership_plan.md "actor-model
    // relationship"): load/store pc -> the host engine field the base register (an engine HANDLE)
    // names. At a typed READ the emitter calls a generated `sbget_<T>_<member>(handle)` getter
    // (defined by NAME in the port-world eng_accessors TU) instead of MEM_R* against the handle
    // token; scalar->value, engine-ptr/array->handle, guest-data-ptr->guest ea. A typed WRITE has
    // no setter yet -> routed to a loud runtime trap (never a silent MEM_W). Empty = none.
    std::map<u32, EngField> eng_fields;
};

// One generated virtual-dispatch thunk to define in the port-world thunk TU (decomp headers,
// no cpu_state.h): `extern "C" void <thunk>(u32 h){ ((<type>*)sb_eng_host(h))-><method>(); }`.
struct EmitVirtThunk {
    std::string thunk, type, method;
};

// One generated construction thunk: `extern "C" std::uint32_t sbnew_<T>(){ return
// sb_eng_handle(::operator new(sizeof(T))); }` — a raw host buffer + handle, NO field access.
// Defined in the port-world thunk TU (it needs the decomp sizeof(T)).
struct EmitAllocThunk {
    std::string thunk, type;
};

// One generated bridged-getter for an inline engine-field READ. `decl` is the extern "C"
// prototype (#included by functions.h via eng_accessors.h so the generated game TU can call it);
// `def` is the PORT-WORLD definition that names the host struct member by NAME (so the host C++
// compiler resolves offset/width/endianness/pointer-size). main.cpp writes both into
// generated/eng_accessors.{h,cpp}.
struct EmitEngGetter {
    std::string thunk, decl, def;
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

    // Construction thunks emitted across all functions (deduped) — main.cpp writes their
    // definitions into the port-world thunk TU alongside the virtual-dispatch thunks.
    const std::vector<EmitAllocThunk>& alloc_thunks() const { return alloc_thunks_; }

    // Bridged getters for inline engine-field reads emitted across all functions (deduped) —
    // main.cpp writes their port-world definitions into generated/eng_accessors.cpp.
    const std::vector<EmitEngGetter>& eng_getters() const { return eng_getters_; }

private:
    std::ostream& out_;
    int unhandled_ = 0;
    std::vector<std::string> unhandled_ops_;
    std::vector<EmitVirtThunk> virt_thunks_;
    std::unordered_set<std::string> virt_thunk_seen_;
    std::vector<EmitAllocThunk> alloc_thunks_;
    std::unordered_set<std::string> alloc_thunk_seen_;
    std::vector<EmitEngGetter> eng_getters_;
    std::unordered_set<std::string> eng_getter_seen_;

    void emit_instr(const PPCInstr& i, const EmitContext& ctx);

    // Bridged-getter emission for an inline engine-field access (ctx.eng_fields). Returns true
    // (and emits the getter call / write trap) when `i` is a registered typed site; false to fall
    // through to the normal guest MEM emit. eng_get_symbol builds + records the getter thunk.
    bool emit_eng_field(const PPCInstr& i, const EmitContext& ctx);
    std::string eng_get_symbol(const EngField& f, PPCOp op);

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
