// cover_fruit_test — spec-constant test for the TCoverFruit::loadAfter port's
// TFlagManager flag id. Guards against a one-hex-digit typo that would silently break the
// "collected fruit stays gone" invariant across save loads.
//
// Spec (from disasm of 0x801e1748):
//   loadAfter reads the flag via `lis r4, 1; addi r4, r4, 0x38B` → 0x1038B. If getBool
//   returns non-zero, TCoverFruit::loadAfter dispatches vtable slot 0x104 which resolves
//   to TMapObjBase::makeObjDead (`or 0x10 into mLiveFlag` + zero velocity), killing the
//   object at load time so a re-entered stage doesn't respawn the fruit.

#include "sms_boot_coverfruit.h"
#include <cstdio>
#include <cstdint>

int main() {
	int fail = 0;
	if (sb::kCoverFruitCollectedFlag != 0x1038Bu) {
		std::fprintf(stderr, "FAIL: kCoverFruitCollectedFlag != 0x1038B\n");
		++fail;
	}
	// Adjacency check: 0x1038A / 0x1038C are the two most-likely typo targets — both are
	// LIVE flag ids used elsewhere (0x1038C is set in MapObjBlock.cpp), so a wrong id would
	// alias a real flag rather than a dead one and produce a silent behavior change.
	if (sb::kCoverFruitCollectedFlag == 0x1038Au) { std::fprintf(stderr, "FAIL: 0x1038A alias\n"); ++fail; }
	if (sb::kCoverFruitCollectedFlag == 0x1038Cu) { std::fprintf(stderr, "FAIL: 0x1038C alias\n"); ++fail; }
	if (fail) return 1;
	std::printf("cover_fruit_test: all passed\n");
	return 0;
}
