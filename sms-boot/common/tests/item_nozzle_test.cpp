// item_nozzle_test — spec-derived unit test for the three small ports:
//   TItemNozzle::put, TItemNozzle::appearing, TNozzleBox::control.
// Pure logic, Dolphin-free / no ROM.

#include "sms_boot_itemnozzle.h"
#include <cstdio>
#include <cstdint>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
	std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

int main() {
	// --- Bit constant sanity ---
	CHECK(sb::kLiveFlagUnk10 == 0x10, "LIVE_FLAG_UNK10 mask value");
	CHECK(sb::kHitActorUnk64Bit0 == 0x1, "HitActor unk64 bit-0 value");

	// --- TItemNozzle::put ---
	{
		sb::ItemNozzleFields s{ /*unk64*/ 0xFFFF'FFFFu, /*live*/ 0, /*state*/ 0 };
		sb::item_nozzle_put(s);
		CHECK(s.unk64 == 0xFFFF'FFFEu, "put: unk64 clears LSB and keeps top bits");
		CHECK(s.state == 1,           "put: state = 1");
	}
	{
		// Pre-cleared bit stays cleared, other bits stay.
		sb::ItemNozzleFields s{ /*unk64*/ 0xDEADBEE0u, /*live*/ 0, /*state*/ 42 };
		sb::item_nozzle_put(s);
		CHECK(s.unk64 == 0xDEADBEE0u, "put: unk64 unchanged when LSB already clear");
		CHECK(s.state == 1,           "put: state overwritten unconditionally");
	}

	// --- TItemNozzle::appearing ---
	{
		// Guard OFF: no mutations.
		sb::ItemNozzleFields s{ /*unk64*/ 0xAAAA'AAABu, /*live*/ 0x000, /*state*/ 5 };
		sb::item_nozzle_appearing(s);
		CHECK(s.unk64 == 0xAAAA'AAABu, "appearing: unk64 untouched when guard fails");
		CHECK(s.state == 5,           "appearing: state untouched when guard fails");
	}
	{
		// Other unrelated flag bits set but not 0x10 → still fails guard.
		sb::ItemNozzleFields s{ /*unk64*/ 0x1u, /*live*/ 0xFFFF'FFEFu, /*state*/ 5 };
		sb::item_nozzle_appearing(s);
		CHECK(s.state == 5, "appearing: guard tests ONLY bit 0x10, not any-nonzero");
		CHECK(s.unk64 == 0x1u, "appearing: unk64 untouched when 0x10 absent");
	}
	{
		// Guard ON: mutations happen.
		sb::ItemNozzleFields s{ /*unk64*/ 0x11u, /*live*/ 0x10u, /*state*/ 5 };
		sb::item_nozzle_appearing(s);
		CHECK(s.unk64 == 0x10u, "appearing: unk64 clears LSB when guard passes");
		CHECK(s.state == 1,     "appearing: state = 1 when guard passes");
	}

	// --- TNozzleBox::control tail ---
	{
		// unk148 == 4 short-circuit BEFORE unk166 read (order matters).
		sb::NozzleBoxFields s{ /*unk148*/ 4, /*col*/ 0, /*unk166*/ 1 };
		sb::nozzle_box_control_tail(s);
		CHECK(s.unk166 == 1, "control: unk148==4 returns without touching unk166");
	}
	{
		// unk166 == 0 → early return.
		sb::NozzleBoxFields s{ /*unk148*/ 3, /*col*/ 0, /*unk166*/ 0 };
		sb::nozzle_box_control_tail(s);
		CHECK(s.unk166 == 0, "control: unk166==0 stays 0");
	}
	{
		// col_count nonzero → don't clear.
		sb::NozzleBoxFields s{ /*unk148*/ 3, /*col*/ 1, /*unk166*/ 1 };
		sb::nozzle_box_control_tail(s);
		CHECK(s.unk166 == 1, "control: colliding this tick keeps unk166 set");
	}
	{
		// All three guards pass → clear unk166.
		sb::NozzleBoxFields s{ /*unk148*/ 0, /*col*/ 0, /*unk166*/ 1 };
		sb::nozzle_box_control_tail(s);
		CHECK(s.unk166 == 0, "control: quiescent tick clears unk166");
	}
	{
		// unk148 < 0 is NOT the sentinel — only exact ==4 short-circuits.
		sb::NozzleBoxFields s{ /*unk148*/ -1, /*col*/ 0, /*unk166*/ 1 };
		sb::nozzle_box_control_tail(s);
		CHECK(s.unk166 == 0, "control: unk148==-1 is not the sentinel (only ==4 is)");
	}

	if (g_fail) { std::fprintf(stderr, "item_nozzle_test: %d FAILURE(S)\n", g_fail); return 1; }
	std::printf("item_nozzle_test: all passed\n");
	return 0;
}
