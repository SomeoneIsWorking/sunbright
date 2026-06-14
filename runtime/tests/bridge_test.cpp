// Unit test for runtime/bridge.h — the native<->recomp marshalling thunk.
//
// Self-contained: stubs the three runtime externs the thunk depends on
// (register_override + override_lookup, sb_guest_to_host, call_ppc) so the
// EABI marshalling logic is tested in isolation, with no Dolphin / no recomp.
// Build: see runtime/tests/run_bridge_test.sh (plain g++, C++17).
//
// What it proves: integer/pointer/float args land in the right host parameters
// per the PPC EABI (int/ptr in r3.. and float in f1.. as INDEPENDENT sequences),
// pointer args are translated guest->host, results land in r3 / f1, void calls
// run for side effects, and the thunk returns via lr.
#include "../bridge.h"

#include <cstdio>
#include <cstring>
#include <map>

// ---- stubs for the three runtime externs the bridge needs -------------------
static std::map<u32, RecompFunc> g_overrides;
void register_override(u32 addr, RecompFunc fn) { g_overrides[addr] = fn; }
RecompFunc override_lookup(u32 addr) {
	auto it = g_overrides.find(addr);
	return it == g_overrides.end() ? nullptr : it->second;
}

static unsigned char g_guest_ram[0x2000000];      // 32 MB fake guest RAM
void* sb_guest_to_host(u32 ea) {
	return ea ? (g_guest_ram + (ea & 0x01FFFFFFu)) : nullptr;
}

// Minimal engine-handle table stub (the real one is runtime/eng_handle.cpp).
#include <vector>
static std::vector<void*> g_eng = { nullptr };
void* sb_eng_host(u32 h)    { return h && h < g_eng.size() ? g_eng[h] : nullptr; }
u32   sb_eng_handle(void* p){ if (!p) return 0; g_eng.push_back(p); return (u32)g_eng.size() - 1; }

static u32 g_last_call_ppc = 0;
void call_ppc(CPUState& cpu, u32 address) { (void)cpu; g_last_call_ppc = address; }

// ---- functions under bridge (host-native, normal C++ signatures) ------------
static int      add3(int a, int b, int c)                 { return a + b + c; }
static u32      sum_bytes(const u8* p, u32 n)             { u32 s = 0; for (u32 i = 0; i < n; i++) s += p[i]; return s; }
static double   mix(int a, double b, int c, double d)     { return a + b + c + d; }
static void     store32(u32* p, u32 v)                    { *p = v; }

// host-native ENGINE object: a pointer to it crosses the boundary as a HANDLE.
struct FakeTex { int w; int h; };
SB_ENGINE_TYPE(FakeTex);
// method-like: `this` (engine handle) + a guest-data pointer arg, mixed in one call.
static u32      tex_fill(FakeTex* t, u8* buf)             { for (int i=0;i<t->w*t->h;i++) buf[i]=0xAB; return (u32)(t->w*t->h); }
// factory: returns a host engine object -> must come back as a handle.
static FakeTex* tex_make(int w, int h)                   { static FakeTex pool[4]; static int n=0; FakeTex* t=&pool[n++]; t->w=w; t->h=h; return t; }

static const u32 A_ADD3   = 0x80010000;
static const u32 A_SUM    = 0x80010100;
static const u32 A_MIX    = 0x80010200;
static const u32 A_STORE  = 0x80010300;
static const u32 A_TEXFILL= 0x80010400;
static const u32 A_TEXMAKE= 0x80010500;

SUNBRIGHT_BRIDGE(A_ADD3,   &add3);
SUNBRIGHT_BRIDGE(A_SUM,    &sum_bytes);
SUNBRIGHT_BRIDGE(A_MIX,    &mix);
SUNBRIGHT_BRIDGE(A_STORE,  &store32);
SUNBRIGHT_BRIDGE(A_TEXFILL,&tex_fill);
SUNBRIGHT_BRIDGE(A_TEXMAKE,&tex_make);

static int g_fail = 0;
#define CHECK(cond, msg) do { \
	if (cond) std::printf("[bridge_test]   ok: %s\n", msg); \
	else    { std::printf("[bridge_test] FAIL: %s\n", msg); g_fail = 1; } \
} while (0)

static void run(u32 addr, CPUState& cpu) {
	g_last_call_ppc = 0;
	cpu.lr = 0xDEADBEEF;
	RecompFunc fn = override_lookup(addr);
	if (!fn) { std::printf("[bridge_test] FAIL: no override at %08x\n", addr); g_fail = 1; return; }
	fn(cpu);
	CHECK(g_last_call_ppc == 0xDEADBEEF, "thunk returned via lr (call_ppc(lr))");
}

int main() {
	std::setbuf(stdout, nullptr);

	// add3(10,20,30) -> r3
	{ CPUState cpu; cpu.reset();
	  cpu.gpr[3] = 10; cpu.gpr[4] = 20; cpu.gpr[5] = 30;
	  run(A_ADD3, cpu);
	  CHECK(cpu.gpr[3] == 60, "int args r3..r5 marshalled; return in r3"); }

	// sum_bytes(ptr,n): pointer arg translated guest->host, reads guest RAM
	{ CPUState cpu; cpu.reset();
	  u32 ea = 0x80001000; u8* host = (u8*)sb_guest_to_host(ea);
	  u32 want = 0; for (int i = 0; i < 16; i++) { host[i] = (u8)(i * 7 + 1); want += host[i]; }
	  cpu.gpr[3] = ea; cpu.gpr[4] = 16;
	  run(A_SUM, cpu);
	  CHECK(cpu.gpr[3] == want, "pointer arg translated guest->host; bytes summed"); }

	// mix(int a, double b, int c, double d): the EABI bank-separation test.
	// a->r3, c->r4 (ints); b->f1, d->f2 (floats) — independent sequences.
	{ CPUState cpu; cpu.reset();
	  cpu.gpr[3] = 1; cpu.gpr[4] = 3;
	  cpu.fpr[1].ps0 = 2.0; cpu.fpr[2].ps0 = 4.0;
	  run(A_MIX, cpu);
	  CHECK(cpu.fpr[1].ps0 == 10.0, "mixed int/float args split across r3,r4 / f1,f2; fp return in f1"); }

	// store32: void return, writes through translated pointer into guest RAM
	{ CPUState cpu; cpu.reset();
	  u32 ea = 0x80002000;
	  cpu.gpr[3] = ea; cpu.gpr[4] = 0xCAFEF00D;
	  run(A_STORE, cpu);
	  CHECK(*(u32*)sb_guest_to_host(ea) == 0xCAFEF00D, "void fn ran; wrote through translated pointer"); }

	// engine-pointer FACTORY: returned host object comes back as a handle in r3
	// that round-trips through sb_eng_host back to the same host object.
	u32 tex_handle;
	{ CPUState cpu; cpu.reset();
	  cpu.gpr[3] = 5; cpu.gpr[4] = 4;     // tex_make(5,4)
	  run(A_TEXMAKE, cpu);
	  tex_handle = cpu.gpr[3];
	  FakeTex* host = (FakeTex*)sb_eng_host(tex_handle);
	  CHECK(host && host->w == 5 && host->h == 4, "engine-ptr return -> handle in r3, round-trips to host obj"); }

	// engine-method: `this` arrives as a HANDLE (sb_eng_host), a SECOND pointer arg
	// is a guest-data pointer (sb_guest_to_host) — the two pointer kinds must be
	// discriminated by type in the SAME call.
	{ CPUState cpu; cpu.reset();
	  u32 ea = 0x80003000;
	  cpu.gpr[3] = tex_handle;            // this = engine handle (5*4=20 texels)
	  cpu.gpr[4] = ea;                    // buf = guest pointer
	  run(A_TEXFILL, cpu);
	  u8* buf = (u8*)sb_guest_to_host(ea);
	  CHECK(cpu.gpr[3] == 20 && buf[0] == 0xAB && buf[19] == 0xAB,
	        "engine handle (this) and guest pointer (buf) marshalled distinctly in one call"); }

	if (g_fail) { std::printf("[bridge_test] RESULT: FAIL\n"); return 1; }
	std::printf("[bridge_test] RESULT: PASS — EABI marshalling thunk correct\n");
	return 0;
}
