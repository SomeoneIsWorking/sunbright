// OBJECT-IDENTITY slice — end-to-end runtime proof (docs/re_notes/object_identity.md).
//
// Proves engine-CONSTRUCTION across the boundary: a recompiled `new EngineTex` (Pattern B, inlined
// ctor) builds a HOST-NATIVE object whose typed inlined-ctor writes land in the right host members,
// matching the ORACLE (the same recompiled function over a raw guest-layout buffer = today's
// recompiler). The TAILORED `operator new` is rewritten to sb_eng_alloc<EngineTex>() (host storage +
// handle); the ORACLE calls a real bump allocator into guest RAM.
//
// Self-contained C++17; stubs the runtime externs + a light guest-RAM MEM layer; #includes the
// EMITTER'S OWN output so the real recompiler code path is under test.
// Build/run: runtime/tests/run_construct_slice_test.sh (runs the generator first).
#include "../cpu_state.h"
#include "../bridge.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) std::printf("[construct_slice]   ok: %s\n", msg); \
    else    { std::printf("[construct_slice] FAIL: %s\n", msg); g_fail = 1; } \
} while (0)

// ── override table + call_ppc ─────────────────────────────────────────────────────────
static std::map<u32, RecompFunc> g_overrides;
void register_override(u32 addr, RecompFunc fn) { g_overrides[addr] = fn; }
RecompFunc override_lookup(u32 addr) {
    auto it = g_overrides.find(addr);
    return it == g_overrides.end() ? nullptr : it->second;
}
void call_ppc(CPUState& cpu, u32 address) {
    if (RecompFunc fn = override_lookup(address)) fn(cpu);
}

// ── light guest RAM + big-endian MEM layer (the ORACLE world) ─────────────────────────
static unsigned char g_guest_ram[0x2000000];
static unsigned char* gram(u32 ea) { return g_guest_ram + (ea & 0x01FFFFFFu); }
void* sb_guest_to_host(u32 ea) { return ea ? gram(ea) : nullptr; }
static u32  tst_r32(u32 ea) { u32 b; std::memcpy(&b, gram(ea), 4); return __builtin_bswap32(b); }
static void tst_w32(u32 ea, u32 v) { u32 b = __builtin_bswap32(v); std::memcpy(gram(ea), &b, 4); }
#define MEM_R32(ea)     tst_r32(ea)
#define MEM_W32(ea,v)   tst_w32(ea,v)

// ── ORACLE allocator: a bump allocator handing out guest RAM (today's operator new) ───
static u32 g_bump = 0x80300000u;
static void oracle_operator_new(CPUState& cpu) { cpu.gpr[3] = g_bump; g_bump += 0x100; }

// ── the HOST-NATIVE engine object (the TAILORED world) ────────────────────────────────
// Host offsets DIFFER from the guest displacements (mEmbPalette guest 0x28, mWidth guest 0x3c) —
// a bug that reused the guest offset would write the wrong host member and fail the comparison.
struct EngineTex {
    void* pad;            // host off 0
    int   mWidth;         // host off 8   (guest 0x3c)
    int   mEmbPalette;    // host off 12  (guest 0x28)
};

// ── handle table + sb_eng_alloc (the TAILORED engine-pointer representation) ───────────
static std::vector<void*> g_eng_table = { nullptr };
static std::map<void*, u32> g_eng_index;
void* sb_eng_host(u32 handle) { return (handle < g_eng_table.size()) ? g_eng_table[handle] : nullptr; }
u32 sb_eng_handle(void* host) {
    if (!host) return 0;
    auto it = g_eng_index.find(host);
    if (it != g_eng_index.end()) return it->second;
    u32 h = (u32)g_eng_table.size();
    g_eng_table.push_back(host);
    g_eng_index[host] = h;
    return h;
}
void sb_eng_release(void* host) {                 // release a handle by host pointer
    auto it = g_eng_index.find(host);
    if (it == g_eng_index.end()) return;
    g_eng_table[it->second] = nullptr;
    g_eng_index.erase(it);
}
// Option A: construction is now a CALL to a per-type sbnew_<T>()/sbsizeof_<T>() thunk (defined in
// construct_accessors.inc against the stub structs below); the generated code names no host type.
// SbDynStackObj mirrors runtime/intrinsics.h — RAII host storage sized at runtime + a handle.
struct SbDynStackObj {
    void* storage_;
    u32   h_;
    explicit SbDynStackObj(std::size_t sz) : storage_(::operator new(sz)), h_(sb_eng_handle(storage_)) {}
    ~SbDynStackObj() { sb_eng_release(storage_); ::operator delete(storage_); }
    u32 handle() const { return h_; }
};

// ── the type-revealing engine method (no-op at runtime; only seeds the offline recovery) ─
static void eng_touch(EngineTex*) {}
SUNBRIGHT_BRIDGE(0x80009200u, &eng_touch);

// ── a POLYMORPHIC engine type + its out-of-line PLACEMENT-NEW ctor bridge ─────────────
// EngineTexV has a vtable; raw sb_eng_alloc gives uninitialized storage, so the host vtable is set
// only by running the real C++ ctor. The ctor bridge does placement-new on the raw storage — this
// is the path polymorphic types need (the recompiled/inlined-ctor path can't replay a vtable store).
struct EngineTexV {
    virtual int kind() { return 42; }   // a vtable slot -> proves the host vtable got set
    int mWidth;
};
SB_ENGINE_TYPE(EngineTexV);
// CodeWarrior ctors take `this` and return it; here `this` is the raw sb_eng_alloc storage (a handle
// marshalled to a host ptr by the bridge). Placement-new constructs in place (sets the vtable),
// then the ctor body sets fields; returning `this` round-trips the same handle.
static EngineTexV* sb_ct_EngineTexV(EngineTexV* self, int w) {
    EngineTexV* p = new (self) EngineTexV();   // placement-new on the raw storage -> host vtable
    p->mWidth = w;
    return p;
}
SUNBRIGHT_BRIDGE(0x80009300u, &sb_ct_EngineTexV);

// ── the emitter's own output (the real recompiler code path under test) ───────────────
// Pattern B caller @ ..0000; Pattern A caller @ ..1000; Pattern A out-of-line ctor @ ..2000.
extern "C" void func_80010000(CPUState& cpu);   // ORACLE   B caller
extern "C" void func_80011000(CPUState& cpu);   // ORACLE   A caller
extern "C" void func_80012000(CPUState& cpu);   // ORACLE   A ctor (guest-layout)
extern "C" void func_80013000(CPUState& cpu);   // ORACLE   C stack temp (guest stack)
extern "C" void func_80020000(CPUState& cpu);   // TAILORED B caller
extern "C" void func_80021000(CPUState& cpu);   // TAILORED A caller
extern "C" void func_80022000(CPUState& cpu);   // TAILORED A ctor (host-native)
extern "C" void func_80023000(CPUState& cpu);   // TAILORED C stack temp (host SbStackObj)
extern "C" void func_80024000(CPUState& cpu);   // TAILORED A polymorphic (bridged placement-new ctor)
// Option A: the TAILORED bodies CALL sbnew_<T>/sbsizeof_<T>/sbf_<T>_<field> thunks; their DEFS
// compile here against the EngineTex/EngineTexV stubs above (the port-compiled accessor TU role).
#include "../../scratch/port/construct_accessors.inc"
#include "../../scratch/port/construct_oracle.inc"
#include "../../scratch/port/construct_tailored.inc"

int main() {
    std::setbuf(stdout, nullptr);
    const u32 STACK = 0x80200000u;

    register_override(0x80009100u, &oracle_operator_new);   // operator new (oracle only)
    // The out-of-line ctors are ordinary recompiled functions; call_ppc dispatches to them.
    register_override(0x80012000u, &func_80012000);         // ORACLE   ctor (guest layout)
    register_override(0x80022000u, &func_80022000);         // TAILORED ctor (host-native, recompiled)

    // ── Pattern B (heap new, INLINED ctor) ──
    u32 b_oracle_obj;
    { CPUState cpu; cpu.reset(); cpu.gpr[1] = STACK; func_80010000(cpu); b_oracle_obj = cpu.gpr[3]; }
    u32 b_oracle_width = tst_r32(b_oracle_obj + 0x3c), b_oracle_emb = tst_r32(b_oracle_obj + 0x28);
    EngineTex* b_host = nullptr;
    { CPUState cpu; cpu.reset(); cpu.gpr[1] = STACK; func_80020000(cpu); b_host = (EngineTex*)sb_eng_host(cpu.gpr[3]); }

    CHECK(b_oracle_width == 64 && b_oracle_emb == 0, "B ORACLE: inlined ctor writes land in the guest buffer");
    CHECK(b_host != nullptr, "B TAILORED: operator new produced a HOST object via its handle");
    CHECK(b_host && b_host->mWidth == (int)b_oracle_width,
          "B TAILORED: mWidth matches oracle (host offset != guest offset)");
    CHECK(b_host && b_host->mEmbPalette == (int)b_oracle_emb, "B TAILORED: mEmbPalette matches oracle");

    // ── Pattern A (heap new, OUT-OF-LINE ctor — the recompiled tailored ctor does host writes) ──
    u32 a_oracle_obj;
    { CPUState cpu; cpu.reset(); cpu.gpr[1] = STACK; func_80011000(cpu); a_oracle_obj = cpu.gpr[3]; }
    u32 a_oracle_width = tst_r32(a_oracle_obj + 0x3c);
    EngineTex* a_host = nullptr;
    { CPUState cpu; cpu.reset(); cpu.gpr[1] = STACK; func_80021000(cpu); a_host = (EngineTex*)sb_eng_host(cpu.gpr[3]); }

    std::printf("[construct_slice] B: oracle mWidth=%u host mWidth=%d   A: oracle mWidth=%u host mWidth=%d\n",
                b_oracle_width, b_host ? b_host->mWidth : -1, a_oracle_width, a_host ? a_host->mWidth : -1);

    CHECK(a_oracle_width == 77, "A ORACLE: out-of-line ctor writes mWidth into the guest buffer");
    CHECK(a_host != nullptr, "A TAILORED: out-of-line ctor produced a HOST object via its handle");
    CHECK(a_host && a_host->mWidth == (int)a_oracle_width,
          "A TAILORED: recompiled out-of-line ctor wrote the HOST member (no special ctor bridge needed)");

    // ── Pattern C (stack temporary) — write off addi#1, read back off a re-materialized addi#2
    //    (SAME frame slot -> SAME host object), export to frame+0x100 for comparison. ──
    const u32 EXPORT = STACK + 0x100;
    tst_w32(EXPORT, 0);
    { CPUState cpu; cpu.reset(); cpu.gpr[1] = STACK; func_80013000(cpu); }
    u32 c_oracle = tst_r32(EXPORT);
    tst_w32(EXPORT, 0);
    { CPUState cpu; cpu.reset(); cpu.gpr[1] = STACK; func_80023000(cpu); }
    u32 c_tailored = tst_r32(EXPORT);

    std::printf("[construct_slice] C: oracle exported=%u  tailored exported=%u (expect 88)\n", c_oracle, c_tailored);
    CHECK(c_oracle == 88, "C ORACLE: stack-temp mWidth written+read via the guest stack frame");
    CHECK(c_tailored == 88,
          "C TAILORED: re-materialized addi shares ONE host SbStackObj — write off addi#1 read back off addi#2");
    CHECK(c_tailored == c_oracle, "C: tailored stack-temp value matches the oracle");

    // ── Pattern A, POLYMORPHIC — sb_eng_alloc (raw) + a bridged placement-new ctor sets the host
    //    vtable. Tailored-only (the oracle world would marshal a guest ptr into the bridge). ──
    EngineTexV* av_host = nullptr;
    { CPUState cpu; cpu.reset(); cpu.gpr[1] = STACK; func_80024000(cpu); av_host = (EngineTexV*)sb_eng_host(cpu.gpr[3]); }
    std::printf("[construct_slice] AV: host=%p kind()=%d mWidth=%d (expect kind 42, mWidth 99)\n",
                (void*)av_host, av_host ? av_host->kind() : -1, av_host ? av_host->mWidth : -1);
    CHECK(av_host != nullptr, "AV: polymorphic ctor bridge produced a HOST object via its handle");
    CHECK(av_host && av_host->kind() == 42,
          "AV: the bridged PLACEMENT-NEW ctor set the host VTABLE (virtual call resolves)");
    CHECK(av_host && av_host->mWidth == 99, "AV: the ctor set the field");

    if (g_fail) { std::printf("[construct_slice] RESULT: FAIL\n"); return 1; }
    std::printf("[construct_slice] RESULT: PASS — host-native engine construction verified (heap A+B, stack C, polymorphic AV)\n");
    return 0;
}
