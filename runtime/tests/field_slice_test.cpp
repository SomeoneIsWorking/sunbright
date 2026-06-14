// TAILORED-RECOMP de-risk slice — the GATING EXPERIMENT (docs/ARCHITECTURE_TARGET.md).
//
// Proves the HARD, least-proven half of the game<->engine boundary: that recompiled
// game code can read AND write a field of a HOST-NATIVE PC-engine object with correct
// host layout/endianness/pointer-width, AND call a real engine function through the
// boundary — verified to match the ORACLE (the same recompiled function over a raw
// guest-layout, big-endian image, i.e. what today's recompiler / Dolphin produces).
//
// The function-call half is already proven (runtime/bridge.h, run_bridge_test.sh). This
// test combines BOTH halves in one real recompiled function (field read -> engine call
// -> field write) so the slice is end-to-end, not just the field access in isolation.
//
// Self-contained, plain C++17: stubs the runtime externs and provides a light guest-RAM
// MEM layer; pulls in the EMITTER'S OWN output (the two generated .inc bodies) so the
// real recompiler code path is what's under test, not a hand-written mock.
//
// Build/run: runtime/tests/run_field_slice_test.sh (runs the generator first).

#include "../cpu_state.h"
#include "../bridge.h"     // SUNBRIGHT_BRIDGE + the override-table externs it needs

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) std::printf("[field_slice]   ok: %s\n", msg); \
    else    { std::printf("[field_slice] FAIL: %s\n", msg); g_fail = 1; } \
} while (0)

// ── override table + call_ppc (the proven call half) ─────────────────────────────────
static std::map<u32, RecompFunc> g_overrides;
void register_override(u32 addr, RecompFunc fn) { g_overrides[addr] = fn; }
RecompFunc override_lookup(u32 addr) {
    auto it = g_overrides.find(addr);
    return it == g_overrides.end() ? nullptr : it->second;
}
// Inline-continue call model: run the target (here, always a bridged engine override)
// and RETURN so the caller continues inline. A non-override target (e.g. the bridge
// thunk's terminal call_ppc(lr) "return") is a no-op in this isolated test.
void call_ppc(CPUState& cpu, u32 address) {
    if (RecompFunc fn = override_lookup(address)) fn(cpu);
}

// ── light guest RAM + big-endian MEM layer (the ORACLE world) ────────────────────────
static unsigned char g_guest_ram[0x2000000];                 // 32 MB fake guest RAM
static unsigned char* gram(u32 ea) { return g_guest_ram + (ea & 0x01FFFFFFu); }
void* sb_guest_to_host(u32 ea) { return ea ? gram(ea) : nullptr; }

static u32  tst_r32(u32 ea) { u32 b; std::memcpy(&b, gram(ea), 4); return __builtin_bswap32(b); }
static void tst_w32(u32 ea, u32 v) { u32 b = __builtin_bswap32(v); std::memcpy(gram(ea), &b, 4); }
static f32 tst_rf32(u32 ea) { u32 b = tst_r32(ea); f32 v; std::memcpy(&v, &b, 4); return v; }
static void tst_wf32(u32 ea, f32 v) { u32 b; std::memcpy(&b, &v, 4); tst_w32(ea, b); }
#define MEM_R32(ea)     tst_r32(ea)
#define MEM_W32(ea,v)   tst_w32(ea,v)
#define MEM_RF32(ea)    tst_rf32(ea)
#define MEM_WF32(ea,v)  tst_wf32(ea,v)

// ── the HOST-NATIVE engine object (the TAILORED world) ───────────────────────────────
// Modeled on a real JSystem object: a vtable pointer + a pointer member, then the
// scalar fields. Because the pointer members are 8 bytes on the host, mFov lands at host
// offset 16 even though its GUEST offset is 8 — so a bug that reused the guest offset (or
// byte-swapped the value) would be caught.
struct EngineCam {
    void*      vtable;   // host off 0  (8 bytes)
    EngineCam* mNext;    // host off 8  (8 bytes) — a real HOST pointer (NOT a handle)
    float      mFov;     // host off 16
    int        mFlags;   // host off 20
};
static_assert(offsetof(EngineCam, mFov) == 16, "host layout: mFov must differ from guest offset 8");
static_assert(offsetof(EngineCam, mNext) == 8, "host layout: mNext is a real 8-byte host pointer");

// Bidirectional handle table — the slice's chosen engine-pointer representation. A 32-bit
// HANDLE lives in the recomp register file / guest RAM; the host object lives in host
// memory. sb_eng_handle() allocates a stable handle for a host pointer (so a loaded
// engine-pointer FIELD becomes a handle that round-trips through a 32-bit register), and
// sb_eng_host() is its inverse. This is what makes obj->next->field chains work without
// widening the register file to 64-bit.
static std::vector<void*> g_eng_table = { nullptr };   // index 0 reserved == null handle
static std::map<void*, u32> g_eng_index;
void* sb_eng_host(u32 handle) {
    return (handle < g_eng_table.size()) ? g_eng_table[handle] : nullptr;
}
u32 sb_eng_handle(void* host) {
    if (!host) return 0;
    auto it = g_eng_index.find(host);
    if (it != g_eng_index.end()) return it->second;
    u32 h = (u32)g_eng_table.size();
    g_eng_table.push_back(host);
    g_eng_index[host] = h;
    return h;
}

// ── the engine function called through the boundary ──────────────────────────────────
// Layout-agnostic scalar so ORACLE and TAILORED run the SAME host code.
static double eng_scale(double v) { return v * 2.0; }
SUNBRIGHT_BRIDGE(0x80009000u, &eng_scale);

// ── the emitter's own output (the real recompiler code path under test) ──────────────
extern "C" void func_80010000(CPUState& cpu);   // ORACLE   (guest-layout MEM access)
extern "C" void func_80020000(CPUState& cpu);   // TAILORED (host-native field access)
#include "../../scratch/port/slice_oracle.inc"
#include "../../scratch/port/slice_tailored.inc"

int main() {
    std::setbuf(stdout, nullptr);

    const float kNestedFov = 3.5f;         // this->mNext->mFov (not byte-palindromic)
    const float kExpect    = kNestedFov*2; // eng_scale doubles it -> 7.0; written to this->mFov
    const u32   STACK      = 0x80200000u;  // a valid guest stack address (for the spill/reload)

    // ORACLE world: two guest-layout objects in guest RAM (this @ A, this->mNext @ B).
    const u32 A = 0x80100000u, Bn = 0x80101000u;
    tst_w32 (A + 4, Bn);                    // this->mNext = guest pointer to B
    tst_wf32(Bn + 8, kNestedFov);           // B.mFov = 3.5
    tst_wf32(A + 8, 0.0f);                   // this->mFov starts 0
    {
        CPUState cpu; cpu.reset();
        cpu.gpr[1] = STACK;                  // stack pointer (spill target)
        cpu.gpr[3] = A;                      // this = guest pointer
        func_80010000(cpu);
    }
    float oracle_this_fov = tst_rf32(A + 8);
    float oracle_nested   = tst_rf32(Bn + 8);

    // TAILORED world: two HOST-NATIVE objects; mNext is a real host pointer. r3 = a handle.
    static EngineCam host_a, host_b;
    host_a = EngineCam{}; host_b = EngineCam{};
    host_a.mNext = &host_b;                  // real host pointer (8 bytes)
    host_b.mFov  = kNestedFov;
    host_a.mFov  = 0.0f;
    {
        CPUState cpu; cpu.reset();
        cpu.gpr[1] = STACK;                  // same guest stack (the handle spills here as a u32)
        cpu.gpr[3] = sb_eng_handle(&host_a); // this = engine handle
        func_80020000(cpu);
    }
    float tailored_this_fov = host_a.mFov;
    float tailored_nested   = host_b.mFov;

    std::printf("[field_slice] oracle this->mFov=%.3f (nested %.3f)  tailored this->mFov=%.3f (nested %.3f)  expected=%.3f\n",
                oracle_this_fov, oracle_nested, tailored_this_fov, tailored_nested, kExpect);

    CHECK(oracle_this_fov == kExpect,      "ORACLE: spill -> nested-ptr chase -> engine call -> reload -> field write (guest)");
    CHECK(tailored_this_fov == kExpect,    "TAILORED: same against HOST-NATIVE nested objects (handle chain + spill/reload)");
    CHECK(tailored_this_fov == oracle_this_fov, "TAILORED field translation matches the ORACLE (the gating result)");
    CHECK(tailored_nested == oracle_nested,     "the nested object's field was READ, not clobbered, on both sides");

    if (g_fail) { std::printf("[field_slice] RESULT: FAIL\n"); return 1; }
    std::printf("[field_slice] RESULT: PASS — tailored host field access verified vs oracle\n");
    return 0;
}
