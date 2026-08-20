// Spec-derived close test for TGuide::perform's open/close spine (US GMSE01 0x801791d0).
#include "sms_boot_guide.h"
#include <cstdio>

static int g_fail;
#define CHECK(c, m) do { if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++g_fail; } } while (0)

int main()
{
	using namespace sb::guide;
	Transition t = step_transition(9, false, true, false, false);
	CHECK(t.next_state == 9 && t.wipe == kWipeNone, "state 9 waits for guide load");
	t = step_transition(9, true, false, false, false);
	CHECK(t.next_state == 9, "state 9 waits for fully faded out");
	t = step_transition(9, true, true, false, false);
	CHECK(t.next_state == 10 && t.wipe == kWipeIn5, "state 9 starts wipe-in 5");
	t = step_transition(10, true, false, false, false);
	CHECK(t.next_state == 10, "state 10 waits for fully faded in");
	t = step_transition(10, true, false, true, false);
	CHECK(t.next_state == 0 && t.clear_selection, "state 10 becomes interactive");
	t = step_transition(0, true, false, true, true);
	CHECK(t.next_state == 7 && t.wipe == kWipeNone, "close request selects state 7 only");
	t = step_transition(7, true, false, true, false);
	CHECK(t.next_state == 11 && t.wipe == kWipeOut6, "next frame starts wipe-out 6");
	t = step_transition(11, true, false, false, false);
	CHECK(t.next_state == 11 && !t.return_to_gameplay, "state 11 waits for black");
	t = step_transition(11, true, true, false, false);
	CHECK(t.next_state == 8 && t.wipe == kWipeIn5 && t.return_to_gameplay,
	      "state 11 returns ownership to director");
	if (!g_fail) std::puts("guide_transition_test: all checks passed");
	return g_fail ? 1 : 0;
}
