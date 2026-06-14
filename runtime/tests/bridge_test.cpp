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

static u32 g_last_call_ppc = 0;
void call_ppc(CPUState& cpu, u32 address) { (void)cpu; g_last_call_ppc = address; }

// ---- functions under bridge (host-native, normal C++ signatures) ------------
static int      add3(int a, int b, int c)                 { return a + b + c; }
static u32      sum_bytes(const u8* p, u32 n)             { u32 s = 0; for (u32 i = 0; i < n; i++) s += p[i]; return s; }
static double   mix(int a, double b, int c, double d)     { return a + b + c + d; }
static void     store32(u32* p, u32 v)                    { *p = v; }

static const u32 A_ADD3   = 0x80010000;
static const u32 A_SUM    = 0x80010100;
static const u32 A_MIX    = 0x80010200;
static const u32 A_STORE  = 0x80010300;

SUNBRIGHT_BRIDGE(A_ADD3,  &add3);
SUNBRIGHT_BRIDGE(A_SUM,   &sum_bytes);
SUNBRIGHT_BRIDGE(A_MIX,   &mix);
SUNBRIGHT_BRIDGE(A_STORE, &store32);

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

	if (g_fail) { std::printf("[bridge_test] RESULT: FAIL\n"); return 1; }
	std::printf("[bridge_test] RESULT: PASS — EABI marshalling thunk correct\n");
	return 0;
}
