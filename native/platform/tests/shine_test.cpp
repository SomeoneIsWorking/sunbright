// shine_test — spec-constant test for TShine::initMapObj (@0x801bcd70).
// Pure logic, Dolphin-free / no ROM.

#include "sms_boot_shine.h"
#include <cstdio>
#include <cstdint>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
	std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

int main() {
	CHECK(sb::shine_init::kUnk14C == 0x1e0, "unk14C = 0x1e0 (480)");
	CHECK(sb::shine_init::kUnk150 == 0x78,  "unk150 = 0x78 (120)");
	CHECK(sb::shine_init::kUnk1A4 == 0,     "unk1A4 = 0 (u8)");
	CHECK(sb::shine_init::kUnk1A8 == 0.0f,  "unk1A8 = 0.0f");
	CHECK(sb::shine_init::kUnk1AC == 0.0f,  "unk1AC = 0.0f");
	CHECK(sb::shine_init::kUnk1B0 == 0.0f,  "unk1B0 = 0.0f");
	CHECK(sb::shine_init::kUnk170 == 0xf0,  "unk170 = 0xf0 (240)");
	CHECK(sb::shine_init::kUnk174 == 0,     "unk174 = 0");
	CHECK(sb::shine_init::kUnk178 == 0xf0,  "unk178 = 0xf0 (240)");
	// Invariant: unk170 and unk178 share the same seed value.
	CHECK(sb::shine_init::kUnk170 == sb::shine_init::kUnk178,
	      "unk170 and unk178 seed to identical values");

	// --- TShine::loadAfter dispatch (@0x801bcd08) ---
	using E = sb::shine_load_after::Effect;
	CHECK(sb::shine_load_after::kWaitFrames == 0xf0, "loadAfter: wait-timer = 0xf0 (240)");
	CHECK(sb::shine_load_after::kWaitState  == 0x12, "loadAfter: wait-state = 0x12 (18)");
	CHECK(sb::shine_load_after::classify(0) == E::NoOp,               "loadAfter: unk154==0 → no-op");
	CHECK(sb::shine_load_after::classify(1) == E::MakeObjDead,        "loadAfter: unk154==1 → makeObjDead");
	CHECK(sb::shine_load_after::classify(2) == E::WaitTimerAndSet,    "loadAfter: unk154==2 → wait-timer branch");
	CHECK(sb::shine_load_after::classify(3) == E::NoOp,               "loadAfter: unk154>=3 → no-op");
	CHECK(sb::shine_load_after::classify(0xFFFFFFFFu) == E::NoOp,     "loadAfter: overflow-large unk154 → no-op");

	// --- TShine::loadBeforeInit predicates (@0x801bcc18) ---
	{
		using namespace sb::shine_load_before_init;
		CHECK(name_to_unk154("normal")   == 0, "loadBeforeInit: 'normal' → 0");
		CHECK(name_to_unk154("quickly")  == 2, "loadBeforeInit: 'quickly' → 2");
		CHECK(name_to_unk154("")         == 1, "loadBeforeInit: empty → 1");
		CHECK(name_to_unk154("Normal")   == 1, "loadBeforeInit: strcmp is case-sensitive");
		CHECK(name_to_unk154("normal ")  == 1, "loadBeforeInit: strcmp requires exact match");
		CHECK(name_to_unk154("norm")     == 1, "loadBeforeInit: prefix does NOT match");
		CHECK(name_to_unk154(nullptr)    == 1, "loadBeforeInit: null-guard falls to 1");

		CHECK(kDefaultWait == 0x78, "loadBeforeInit: default wait = 0x78 (120)");
		CHECK(clamp_wait(-1) == 0x78, "loadBeforeInit: -1 → default 120");
		CHECK(clamp_wait(0)  == 0,    "loadBeforeInit: 0 passthrough");
		CHECK(clamp_wait(60) == 60,   "loadBeforeInit: 60 passthrough");
		CHECK(clamp_wait(-2) == -2,   "loadBeforeInit: -2 passthrough (only -1 is the sentinel)");

		// clamp_toggle: only toggle ∈ {-1, 0} produce nonzero results.
		CHECK(clamp_toggle(-1) == 0, "loadBeforeInit: toggle -1 → 0 (kept, +1)");
		CHECK(clamp_toggle(0)  == 1, "loadBeforeInit: toggle 0 → 1 (kept, +1)");
		CHECK(clamp_toggle(1)  == 0, "loadBeforeInit: toggle 1 → 0 (pinned to -1, +1)");
		CHECK(clamp_toggle(42) == 0, "loadBeforeInit: toggle >1 → 0");
		// Signed: -2 → val+1 = -1 < 2 → kept → 0 (u8 wraparound? -1+1=0). Verify.
		CHECK(clamp_toggle(-2) == 0xFF, "loadBeforeInit: toggle -2 → (u8)(-1) = 0xFF");
	}

	// --- TShine::makeMActors bmd choice (@0x801bcdd4) ---
	{
		using namespace sb::shine_make_mactors;
		CHECK(kKeeperModelLoaderFlags == 0x10220000u,
		      "makeMActors: TMActorKeeper.mModelLoaderFlags = 0x10220000");
		CHECK(kUnk1B4EmptyBranch == 1,
		      "makeMActors: unk1B4 latch is 1 in the empty-pedestal branch");

		// Flag unset → normal shine regardless of name.
		CHECK(choose_bmd(false, nullptr)         == kShineNormalBmd,
		      "makeMActors: flag=0 + null → shine.bmd");
		CHECK(choose_bmd(false, "シャイン（マニ屋用）") == kShineNormalBmd,
		      "makeMActors: flag=0 + Mani-shop name → still shine.bmd (AND check)");
		CHECK(choose_bmd(false, "シャイン")        == kShineNormalBmd,
		      "makeMActors: flag=0 + default name → shine.bmd");

		// Flag set — only the exact Mani-shop name flips to empty.
		CHECK(choose_bmd(true, nullptr)          == kShineNormalBmd,
		      "makeMActors: flag=1 + null name → shine.bmd (null-guard)");
		CHECK(choose_bmd(true, "シャイン")         == kShineNormalBmd,
		      "makeMActors: flag=1 + default name → shine.bmd (not Mani-shop)");
		CHECK(choose_bmd(true, "シャイン（マニ屋用）") == kShineEmptyBmd,
		      "makeMActors: flag=1 + Mani-shop → shine_empty.bmd");

		// Name-substring must NOT match (exact strcmp only).
		CHECK(choose_bmd(true, "マニ屋用")         == kShineNormalBmd,
		      "makeMActors: name substring does not match");
		CHECK(choose_bmd(true, "シャイン（マニ屋用）extra") == kShineNormalBmd,
		      "makeMActors: name suffix invalidates match");

		// Latch predicate mirrors bmd choice.
		CHECK(!should_latch_empty(false, "シャイン（マニ屋用）"),
		      "makeMActors: unk1B4 stays 0 when flag=0");
		CHECK(should_latch_empty(true, "シャイン（マニ屋用）"),
		      "makeMActors: unk1B4 latches when flag+name match");
		CHECK(!should_latch_empty(true, "シャイン"),
		      "makeMActors: unk1B4 stays 0 for the plain-name variant");
	}

	if (g_fail) { std::fprintf(stderr, "shine_test: %d FAILURE(S)\n", g_fail); return 1; }
	std::printf("shine_test: all passed\n");
	return 0;
}
