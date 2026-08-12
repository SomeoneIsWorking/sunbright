// guide_checkpoint_test — spec-derived unit test for TGuide::checkPoint (@0x8017a6bc).
//
// Regressions this catches:
//   * any edge test written non-strict (>= / <=). The disassembly branches to REJECT on
//     ble/bge for all four bounds, so a point ON an edge must MISS. Getting this wrong makes
//     the guide screen's buttons one pixel too generous on every side, which nothing visible
//     would reveal.
//   * the x/y bounds crossed over — the stack slots are read 0x40,0x48 for x and 0x44,0x4c for
//     y, i.e. JUTRect is (x1, y1, x2, y2) and the pairs are (0,2) and (1,3), NOT (0,1) and (2,3).
//   * the validity gate applied to the wrong index range, or inverted. Retail discards a hit in
//     [0,10) when the pass-two pane is INVISIBLE, and leaves every other index alone.
//   * the gate accidentally "fixed" to only apply to pass-two hits. It does not; see the header.

#include "sms_boot_guide.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

int main()
{
	using namespace sb::guide;

	CHECK(kPassOnePanes == 14, "pass one scans 14 panes (cmpwi r26, 0xe)");
	CHECK(kPassTwoPanes == 10, "pass two scans 10 panes (cmpwi r26, 0xa)");

	// A 10..90 x 20..80 box. Interior hits.
	CHECK(point_in_rect(50, 50, 10, 20, 90, 80), "centre is inside");
	CHECK(point_in_rect(11, 21, 10, 20, 90, 80), "just inside the top-left corner");
	CHECK(point_in_rect(89, 79, 10, 20, 90, 80), "just inside the bottom-right corner");

	// Every edge is STRICT — this is the whole point of the test.
	CHECK(!point_in_rect(10, 50, 10, 20, 90, 80), "on the LEFT edge misses");
	CHECK(!point_in_rect(90, 50, 10, 20, 90, 80), "on the RIGHT edge misses");
	CHECK(!point_in_rect(50, 20, 10, 20, 90, 80), "on the TOP edge misses");
	CHECK(!point_in_rect(50, 80, 10, 20, 90, 80), "on the BOTTOM edge misses");
	CHECK(!point_in_rect(10, 20, 10, 20, 90, 80), "the top-left corner itself misses");

	// Outside on each side.
	CHECK(!point_in_rect(9, 50, 10, 20, 90, 80), "left of the box");
	CHECK(!point_in_rect(91, 50, 10, 20, 90, 80), "right of the box");
	CHECK(!point_in_rect(50, 19, 10, 20, 90, 80), "above the box");
	CHECK(!point_in_rect(50, 81, 10, 20, 90, 80), "below the box");

	// x and y must not be swapped: this point is inside a 10..90 x 20..80 box only if the
	// pairs are read (x1,x2) and (y1,y2). If the implementation paired (x1,y1) and (x2,y2)
	// it would reject.
	CHECK(point_in_rect(15, 75, 10, 20, 90, 80), "x and y bounds are not crossed over");

	// A degenerate rect can never be hit, because strict-on-both-sides leaves no interior.
	CHECK(!point_in_rect(10, 20, 10, 20, 10, 20), "an empty rect contains nothing");
	CHECK(!point_in_rect(50, 50, 50, 50, 50, 50), "a zero-area rect contains not even its own point");

	// The validity gate.
	CHECK(gate_hit(-1, false) == -1, "a miss stays a miss");
	CHECK(gate_hit(-1, true) == -1, "a miss is not resurrected by a visible pane");
	CHECK(gate_hit(3, true) == 3, "a hit on a VISIBLE pass-two pane survives");
	CHECK(gate_hit(3, false) == -1, "a hit on an INVISIBLE pass-two pane is discarded");
	CHECK(gate_hit(0, false) == -1, "index 0 is inside the gated range");
	CHECK(gate_hit(9, false) == -1, "index 9 is the last gated index");
	CHECK(gate_hit(10, false) == 10, "index 10 is OUTSIDE the gated range and passes ungated");
	CHECK(gate_hit(13, false) == 13, "a pass-one hit at 13 is never validated");

	if (g_fail == 0) {
		std::printf("guide_checkpoint_test: all checks passed\n");
	}
	return g_fail == 0 ? 0 : 1;
}
