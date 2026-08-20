// sms_boot_guide.h — RE'd spec for TGuide's pointer hit-testing (US GMSE01 0x8017a6bc,
// TGuide::checkPoint). Pure logic, no engine types, so it can be unit-tested with no ROM, no GPU
// and no game link — see sms-boot/shims/tests/guide_checkpoint_test.cpp.
//
// The two details worth pinning are both easy to get wrong by writing what one expects instead of
// what the binary does:
//
//   1. ALL FOUR EDGE TESTS ARE STRICT. The disassembly is `ble/bge` to the REJECT label on each
//      bound, i.e. accept requires x > x1 && x < x2 && y > y1 && y < y2. A point exactly on any
//      edge MISSES. The natural way to write a hit-test — x >= x1 && x <= x2 — is wrong on all
//      four sides, and wrong in a way no screenshot would ever show.
//
//   2. THE VALIDITY GATE READS THE SECOND ARRAY, WHATEVER PASS PRODUCED THE HIT. checkPoint
//      searches 14 panes at this+0x168 first and 10 panes at this+0x44c second, then gates the
//      result on `pane44c[hit]->mVisible` whenever 0 <= hit < 10 — including when `hit` is an
//      index into the FIRST array. That is retail behaviour (the bounds check at 0x8017a7ac is on
//      the index alone, not on which pass set it) and it is reproduced rather than corrected.

#ifndef SMS_BOOT_GUIDE_H
#define SMS_BOOT_GUIDE_H

namespace sb {
namespace guide {

// Number of panes each pass scans. From the loop bounds: `cmpwi r26, 0xe` and `cmpwi r26, 0xa`.
enum { kPassOnePanes = 14, kPassTwoPanes = 10 };

// Retail's strict-on-all-four-sides containment (see note 1 above).
inline bool point_in_rect(int x, int y, int x1, int y1, int x2, int y2)
{
	return x > x1 && x < x2 && y > y1 && y < y2;
}

// The final gate at 0x8017a7ac: an index in [0, kPassTwoPanes) is discarded when the
// corresponding pass-two pane is not visible. Indices outside that range pass through untouched,
// which is how a pass-one hit at index >= 10 survives without ever being validated.
inline int gate_hit(int hit, bool pass_two_pane_visible)
{
	if (hit >= 0 && hit < kPassTwoPanes && !pass_two_pane_visible) {
		return -1;
	}
	return hit;
}

enum WipeCommand { kWipeNone, kWipeIn5, kWipeOut6 };

struct Transition {
	int next_state;
	WipeCommand wipe;
	bool clear_selection;
	bool return_to_gameplay;
};

// One invocation models exactly one pass through retail perform's switch at US 0x80179330.
// State changes never fall through to the newly-selected case until the next frame.
inline Transition step_transition(int state, bool loaded, bool fully_faded_out,
                                  bool fully_faded_in, bool close_requested)
{
	Transition result { state, kWipeNone, false, false };
	switch (state) {
	case 9:
		if (loaded && fully_faded_out)
			result = { 10, kWipeIn5, false, false };
		break;
	case 10:
		if (fully_faded_in)
			result = { 0, kWipeNone, true, false };
		break;
	case 0:
		if (close_requested)
			result.next_state = 7;
		break;
	case 7:
		result = { 11, kWipeOut6, false, false };
		break;
	case 11:
		if (fully_faded_out)
			result = { 8, kWipeIn5, false, true };
		break;
	default:
		break;
	}
	return result;
}

} // namespace guide
} // namespace sb

#endif
