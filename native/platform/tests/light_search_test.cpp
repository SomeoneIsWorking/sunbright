// light_search_test — spec-derived unit test for sb_find_named_index,
// the pure name-search helper backing TLightWithDBSet::makeDrawBuffer × 4
// (@0x802289ac / 0x80228b74 / 0x80228d08 / 0x80228eac).
//
// Regressions this catches:
//   * Empty/null array not returning -1 (SEGV when TLightAry->mLights is
//     nullptr pre scene-load).
//   * Returning LAST match instead of FIRST (guest loop breaks on first).
//   * Off-by-one loop bound (would either skip the last entry or read one past
//     the array).
//   * Missing-needle not returning -1 (would leak Light-Group[0] into every
//     unnamed set).

#include "sms_boot_light_search.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

struct FakeNamed {
	const char* name;
	const char* getName() const { return name; }
};

int main() {
	using sb::light_search::find_named_index;

	FakeNamed arr[] = {
		{"alpha"},
		{"beta"},
		{"gamma"},
		{"beta"},  // duplicate to prove FIRST-match, not last
	};

	CHECK(find_named_index(arr, 4, "alpha") == 0, "first entry hit -> 0");
	CHECK(find_named_index(arr, 4, "gamma") == 2, "middle entry hit -> index");
	CHECK(find_named_index(arr, 4, "beta")  == 1, "duplicate: FIRST wins, not last");
	CHECK(find_named_index(arr, 4, "delta") == -1, "no match -> -1 (never leaks entry 0)");
	CHECK(find_named_index(arr, 0, "alpha") == -1, "count=0 -> -1 (empty array before scene load)");
	CHECK(find_named_index<FakeNamed>(nullptr, 4, "alpha") == -1, "null arr -> -1 (pre scene-load)");
	CHECK(find_named_index(arr, 4, nullptr) == -1, "null needle -> -1 (defensive)");

	// Off-by-one guard: "beta" appears at BOTH index 1 and 3; if the bound were
	// `<= count`, we'd read past the array. Confirm the last valid index (3)
	// is reachable with the correct bound by making it the only match.
	FakeNamed arr2[] = {
		{"alpha"},
		{"alpha"},
		{"alpha"},
		{"omega"},  // only match is at index count-1
	};
	CHECK(find_named_index(arr2, 4, "omega") == 3, "last index reachable (bound is < count, not <= count-1)");

	// Shift-JIS needle exercise (the real needles ARE Shift-JIS on GMSE01).
	FakeNamed sj[] = {
		{"\x91\xbe\x97\x7a\x81\x69\x83\x49\x83\x75\x83\x57\x83\x46\x83\x4e\x83\x67\x81\x6a"},  // 太陽（オブジェクト）
	};
	CHECK(find_named_index(sj, 1,
	    "\x91\xbe\x97\x7a\x81\x69\x83\x49\x83\x75\x83\x57\x83\x46\x83\x4e\x83\x67\x81\x6a") == 0,
	    "Shift-JIS needle exact match");
	CHECK(find_named_index(sj, 1,
	    "\x91\xbe\x97\x7a\x81\x69\x83\x49\x83\x75\x83\x57\x83\x46\x83\x4e\x83\x67") == -1,
	    "Shift-JIS partial (truncated needle) -> no match (strcmp, not strncmp)");

	std::printf("light_search_test: %s (fails=%d)\n", g_fail ? "FAIL" : "PASS", g_fail);
	return g_fail ? 1 : 0;
}
