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
// Mirrors runtime/intrinsics.h: raw host storage + a handle (no ctor — the inlined writes init it).
template <class T> static inline u32 sb_eng_alloc() { return sb_eng_handle(::operator new(sizeof(T))); }

// ── the type-revealing engine method (no-op at runtime; only seeds the offline recovery) ─
static void eng_touch(EngineTex*) {}
SUNBRIGHT_BRIDGE(0x80009200u, &eng_touch);

// ── the emitter's own output (the real recompiler code path under test) ───────────────
extern "C" void func_80010000(CPUState& cpu);   // ORACLE   (guest-layout buffer)
extern "C" void func_80020000(CPUState& cpu);   // TAILORED (host construction)
#include "../../scratch/port/construct_oracle.inc"
#include "../../scratch/port/construct_tailored.inc"

int main() {
    std::setbuf(stdout, nullptr);
    const u32 STACK = 0x80200000u;

    // ORACLE: operator new bump-allocates a guest buffer; inlined writes go to it.
    register_override(0x80009100u, &oracle_operator_new);
    u32 oracle_obj;
    {
        CPUState cpu; cpu.reset();
        cpu.gpr[1] = STACK;
        func_80010000(cpu);
        oracle_obj = cpu.gpr[3];                 // the constructed object (guest ptr)
    }
    u32 oracle_width = tst_r32(oracle_obj + 0x3c);
    u32 oracle_emb   = tst_r32(oracle_obj + 0x28);

    // TAILORED: operator new is rewritten to sb_eng_alloc<EngineTex>(); writes hit the host object.
    EngineTex* host_obj = nullptr;
    {
        CPUState cpu; cpu.reset();
        cpu.gpr[1] = STACK;
        func_80020000(cpu);
        host_obj = (EngineTex*)sb_eng_host(cpu.gpr[3]);   // r3 = the handle for the new object
    }

    std::printf("[construct_slice] oracle: mWidth=%u mEmbPalette=%u   tailored: host=%p mWidth=%d mEmbPalette=%d\n",
                oracle_width, oracle_emb, (void*)host_obj,
                host_obj ? host_obj->mWidth : -1, host_obj ? host_obj->mEmbPalette : -1);

    CHECK(oracle_width == 64 && oracle_emb == 0, "ORACLE: inlined ctor writes land in the guest buffer");
    CHECK(host_obj != nullptr, "TAILORED: operator new produced a HOST object reachable via its handle");
    CHECK(host_obj && host_obj->mWidth == (int)oracle_width,
          "TAILORED: mWidth written to the host member matches the oracle (host offset != guest offset)");
    CHECK(host_obj && host_obj->mEmbPalette == (int)oracle_emb,
          "TAILORED: mEmbPalette written to the host member matches the oracle");

    if (g_fail) { std::printf("[construct_slice] RESULT: FAIL\n"); return 1; }
    std::printf("[construct_slice] RESULT: PASS — host-native engine construction verified vs oracle\n");
    return 0;
}
