// mapobjtree_initeach_test.cpp — unit test from RE for TMapObjTree::initEach
// (reference/sms/src/MoveBG/MapObjTree.cpp), cold-ported 2026-07-15 from US
// GMSE01 0x801f6a64. See debug_journal/2026-07-15_mapobjtree_initmapobj_port_re.md.
//
// initEach is a flat switch on THitActor::mActorType (0x4C) that stamps the
// tree's leaf count + spread/growth constants. This is the most
// transcription-error-prone half of the port (11 SDA2 float literals across 6
// species arms), so it gets a table pin: the expected values are the ground
// truth read off the DOL (arm addresses cited), and the test drives the REAL
// linked initEach over each species and checks every field it writes.
//
// initEach is non-virtual and touches only POD fields (mActorType in, the
// 0x148.. block out) — no vtable, no base state — so a zeroed buffer poked
// through a TMapObjTree* exercises it without constructing the actor. The
// full-game link set (like jaudio_release_test) resolves the class's symbols.

#include <MoveBG/MapObjTree.hpp>
#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); g_fail = 1; } \
	else         { std::fprintf(stderr, "ok:   %s\n", msg); } \
} while (0)

struct SpeciesExpect {
	int  actorType;
	s32  leafCount;
	f32  unk148, unk14C, unk15C, unk160, unk164, unk168;
	const char* label;
};

// Ground truth from the DOL (tools/re/disasm_range.py + tools/dol_sda.py --sda2):
static const SpeciesExpect kExpect[] = {
	// arm @0x801f6aac (0x34 shares the 0x38/palm arm)
	{ 0x40000034, 12, 20.0f, 95.0f, 0.001f, 0.006f, 0.01f, 0.97f, "0x34" },
	// arm @0x801f6ae8
	{ 0x40000035,  8, 20.0f, 100.0f, 0.001f, 0.006f, 0.01f, 0.97f, "0x35" },
	// arm @0x801f6b24
	{ 0x40000036, 12, 50.0f, 100.0f, 0.001f, 0.006f, 0.01f, 0.97f, "0x36" },
	// arm @0x801f6b60
	{ 0x40000037,  8, 95.0f, 60.0f, 0.001f, 0.006f, 0.01f, 0.97f, "0x37" },
	// arm @0x801f6aac (palm)
	{ 0x40000038, 12, 20.0f, 95.0f, 0.001f, 0.006f, 0.01f, 0.97f, "0x38 palm" },
	// arm @0x801f6b9c
	{ 0x40000039,  8, 70.0f, 100.0f, 0.004f, 0.008f, 0.03f, 0.9f, "0x39" },
};

int main()
{
	alignas(TMapObjTree) static unsigned char buf[sizeof(TMapObjTree) + 64];

	for (const SpeciesExpect& e : kExpect) {
		std::memset(buf, 0, sizeof(buf));
		TMapObjTree* t = reinterpret_cast<TMapObjTree*>(buf);
		t->mActorType = e.actorType;
		t->initEach();

		char m[96];
		std::snprintf(m, sizeof(m), "%s leafCount==%d", e.label, e.leafCount);
		CHECK(t->mLeafCount == e.leafCount, m);
		std::snprintf(m, sizeof(m), "%s unk148", e.label); CHECK(t->unk148 == e.unk148, m);
		std::snprintf(m, sizeof(m), "%s unk14C", e.label); CHECK(t->unk14C == e.unk14C, m);
		std::snprintf(m, sizeof(m), "%s unk15C", e.label); CHECK(t->unk15C == e.unk15C, m);
		std::snprintf(m, sizeof(m), "%s unk160", e.label); CHECK(t->unk160 == e.unk160, m);
		std::snprintf(m, sizeof(m), "%s unk164", e.label); CHECK(t->unk164 == e.unk164, m);
		std::snprintf(m, sizeof(m), "%s unk168", e.label); CHECK(t->unk168 == e.unk168, m);
	}

	// Out-of-range actor types are a no-op (DOL blr/bgelr) — mLeafCount stays 0.
	for (int at : { 0x40000033, 0x4000003A }) {
		std::memset(buf, 0, sizeof(buf));
		TMapObjTree* t = reinterpret_cast<TMapObjTree*>(buf);
		t->mActorType = at;
		t->initEach();
		char m[64];
		std::snprintf(m, sizeof(m), "out-of-range 0x%x untouched", at);
		CHECK(t->mLeafCount == 0, m);
	}

	std::fprintf(stderr, g_fail ? "\nRESULT: FAIL\n" : "\nRESULT: PASS\n");
	return g_fail;
}
