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

#include <cstdio>
#include <cstring>
#include <map>

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

static f32 tst_rf32(u32 ea) {
    u32 b; std::memcpy(&b, gram(ea), 4); b = __builtin_bswap32(b);
    f32 v; std::memcpy(&v, &b, 4); return v;
}
static void tst_wf32(u32 ea, f32 v) {
    u32 b; std::memcpy(&b, &v, 4); b = __builtin_bswap32(b);
    std::memcpy(gram(ea), &b, 4);
}
#define MEM_RF32(ea)    tst_rf32(ea)
#define MEM_WF32(ea,v)  tst_wf32(ea,v)

// ── the HOST-NATIVE engine object (the TAILORED world) ───────────────────────────────
// Modeled on a real JSystem object: a vtable pointer + a pointer member, then the
// scalar fields. Because the pointer members are 8 bytes on the host, mFov lands at host
// offset 16 even though its GUEST offset is 8 — so a bug that reused the guest offset (or
// byte-swapped the value) would be caught.
struct EngineCam {
    void*      vtable;   // host off 0  (8 bytes)
    EngineCam* mNext;    // host off 8  (8 bytes)
    float      mFov;     // host off 16
    int        mFlags;   // host off 20
};
static_assert(offsetof(EngineCam, mFov) == 16, "host layout: mFov must differ from guest offset 8");

static EngineCam g_host_cam;

// guest token (a HANDLE — the slice's chosen engine-pointer representation, keeping the
// recomp register file 32-bit) -> host object. Populated at the boundary.
static std::map<u32, void*> g_eng_objs;
void* sb_eng_host(u32 token) {
    auto it = g_eng_objs.find(token);
    return it == g_eng_objs.end() ? nullptr : it->second;
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

    const float kInitFov = 3.5f;          // not byte-palindromic: catches a wrong byteswap
    const float kExpect  = kInitFov * 2;  // eng_scale doubles it -> 7.0

    // ORACLE world: object lives in guest RAM at a guest address, big-endian.
    const u32 GUEST_OBJ = 0x80100000u;
    tst_wf32(GUEST_OBJ + 8, kInitFov);    // guest mFov at guest offset 8
    {
        CPUState cpu; cpu.reset();
        cpu.gpr[3] = GUEST_OBJ;           // this = guest pointer
        func_80010000(cpu);
    }
    float oracle_fov = tst_rf32(GUEST_OBJ + 8);

    // TAILORED world: object is host-native; r3 holds a HANDLE mapped to it.
    const u32 CAM_TOKEN = 0xE0000001u;
    g_eng_objs[CAM_TOKEN] = &g_host_cam;
    g_host_cam = EngineCam{};
    g_host_cam.mFov = kInitFov;
    {
        CPUState cpu; cpu.reset();
        cpu.gpr[3] = CAM_TOKEN;           // this = engine handle
        func_80020000(cpu);
    }
    float tailored_fov = g_host_cam.mFov;

    std::printf("[field_slice] oracle mFov=%.3f  tailored mFov=%.3f  expected=%.3f\n",
                oracle_fov, tailored_fov, kExpect);

    CHECK(oracle_fov == kExpect,           "ORACLE: field read -> engine call -> field write (guest layout)");
    CHECK(tailored_fov == kExpect,         "TAILORED: same, against a HOST-NATIVE engine object");
    CHECK(tailored_fov == oracle_fov,      "TAILORED field translation matches the ORACLE (the gating result)");

    if (g_fail) { std::printf("[field_slice] RESULT: FAIL\n"); return 1; }
    std::printf("[field_slice] RESULT: PASS — tailored host field access verified vs oracle\n");
    return 0;
}
