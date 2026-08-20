// hx_wipe_test.cpp — verify-first unit test for the native Hx_ wipe port
// (sms-boot/runtime/hx_wipe.cpp). Drives the type-12 m-mark wipe state machine
// exactly as TSMSFader/TMovieDirector do and asserts the phase machine advances to DONE
// and opens the THP gate — the "a number moves" gate the boot was stuck behind.
//
// No GPU / no ROM / pure logic (all rendering helpers in the impl are no-ops in STAGE A).

#include <GC2D/hx_wiper.h>
#include <cstdio>
#include <cstdlib>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); g_fail = 1; } \
	else         { std::fprintf(stderr, "ok:   %s\n", msg); } \
} while (0)

// table2 lookup (Hx_GetWipeType): drives the fader's FADING_IN/OUT + FADED_IN/OUT choice.
static void test_get_wipe_type() {
	CHECK(Hx_GetWipeType(12) == 1, "Hx_GetWipeType(12)==1 (type-12 wipe -> FADED_IN)");
	CHECK(Hx_GetWipeType(4)  == 0, "Hx_GetWipeType(4)==0");
	CHECK(Hx_GetWipeType(1)  == 1, "Hx_GetWipeType(1)==1");
	CHECK(Hx_GetWipeType(11) == 0, "Hx_GetWipeType(11)==0");
	CHECK(Hx_GetWipeType(0)  == 0, "Hx_GetWipeType(0)==0");
	CHECK(Hx_GetWipeType(-1) == 0, "Hx_GetWipeType(-1) bounds-safe");
	CHECK(Hx_GetWipeType(99) == 0, "Hx_GetWipeType(99) bounds-safe");
}

// The full opening-movie wipe: startWipe(12) then drive Hx_UpdateWipe per-frame, exactly
// as TSMSFader::drawWipe -> TMovieDirector::direct do, until it reports DONE (3).
static void test_mmark_wipe_completes() {
	Hx_ResetWipe(640, 480);
	// idle before start
	CHECK(Hx_UpdateWipe(1.0f) == 0, "idle Hx_UpdateWipe returns 0");
	CHECK(Hx_MovieStartSyncEx() == 0, "MovieStartSyncEx==0 before a type-12 wipe");

	Hx_StartWipe(12, 0);   // movie 9: gpApplication.mFader->startWipe(12, 0.0f, 0.0f)
	// fader sets FADING_IN because Hx_GetWipeType(12)==1
	CHECK(Hx_GetWipeType(12) == 1, "wipe entered as FADING_IN");

	int  frames    = 0;
	int  seCount   = 0;     // times MovieStartSyncEx returned 1 (play title SE)
	int  thpCount  = 0;     // times it returned 2 (THP gate open)
	int  thpFrame  = -1;
	bool done      = false;

	for (frames = 0; frames < 4000; ++frames) {
		// TMovieDirector::direct() polls the sync each frame while !unk18
		int sync = Hx_MovieStartSyncEx();
		if (sync == 1) seCount++;
		else if (sync == 2) { thpCount++; if (thpFrame < 0) thpFrame = frames; }

		u32 st = Hx_UpdateWipe(1.0f);   // TSMSFader::drawWipe -> Hx_UpdateWipe(mRate)
		if (st == 3) { done = true; break; }
	}

	CHECK(done, "type-12 m-mark wipe reaches state DONE (3) -> fader FADED_IN");
	std::fprintf(stderr, "info: wipe finished in %d frames (seCount=%d thpCount=%d thpFrame=%d)\n",
	             frames, seCount, thpCount, thpFrame);
	CHECK(frames > 100, "completion took a faithful number of frames (not an instant fake)");
	CHECK(frames < 1000, "completion bounded (no runaway / hang)");
	CHECK(seCount == 1, "title SE gate (sync==1) fires exactly once");
	CHECK(thpCount == 1, "THP gate (sync==2) opens exactly once");
	CHECK(thpFrame > 0, "THP gate opens after the wipe is underway (phase>=6)");

	// After DONE, the wipe stays done and the gates are latched off.
	CHECK(Hx_UpdateWipe(1.0f) == 3, "post-DONE Hx_UpdateWipe stays 3");
	CHECK(Hx_MovieStartSyncEx() == 0, "post-DONE MovieStartSyncEx==0 (gates consumed)");
}

// Hx_Circle (the Delfino scene-ENTRY iris wipe, type 1 / type 2) behaviour TDD.
// Spec-truth from the DOL disassembly of Hx_Circle 0x80181ab4: phase 0 arms the timer
// (li 0x1e=30 for wipeType==1 (type 1), li 0x19=25 for wipeType==0 (type 2)) then falls
// straight into the phase-1 run body, which Hx_TimerCountDowns once per Hx_UpdateWipe and
// flips state 2->3 (DONE) exactly when the timer hits 0. So the completion frame count is
// EXACT and falsifiable — a wrong timer constant or a missed/extra countdown moves it.
static void drive_circle(int type, int& frames, bool& done, u32& phaseAtDone) {
	Hx_ResetWipe(640, 480);
	Hx_StartWipe(type, 0);
	done = false; phaseAtDone = 0xffffffff;
	for (frames = 1; frames <= 4000; ++frames) {
		u32 st = Hx_UpdateWipe(1.0f);
		if (st == 3) { done = true; break; }
	}
}

static void test_circle_wipe_completes() {
	// type 1 -> wipeType 1 -> timer 30 -> DONE on the 30th Hx_UpdateWipe.
	int  frames; bool done; u32 phaseDone;
	drive_circle(1, frames, done, phaseDone);
	CHECK(done, "type-1 Hx_Circle wipe reaches state DONE (3)");
	CHECK(frames == 30, "type-1 iris completes in EXACTLY 30 frames (li 0x1e)");
	CHECK(Hx_UpdateWipe(1.0f) == 3, "type-1 post-DONE Hx_UpdateWipe stays 3");

	// type 2 -> wipeType 0 -> timer 25 -> DONE on the 25th Hx_UpdateWipe.
	drive_circle(2, frames, done, phaseDone);
	CHECK(done, "type-2 Hx_Circle wipe reaches state DONE (3)");
	CHECK(frames == 25, "type-2 iris completes in EXACTLY 25 frames (li 0x19)");

	// Sensitivity: an UNSTARTED circle is idle (0), never a phantom DONE. And the wipe must
	// NOT complete in a single frame (the wedge bug was the opposite — it never completed;
	// an instant fake-complete would be the equally-wrong other failure).
	Hx_ResetWipe(640, 480);
	CHECK(Hx_UpdateWipe(1.0f) == 0, "unstarted circle is idle (0), not a phantom DONE");
	Hx_StartWipe(1, 0);
	CHECK(Hx_UpdateWipe(1.0f) == 2, "type-1 is RUNNING (2) after frame 1, not instant-DONE");
}

// Hx_Test5 0x8017df74 backs wipe types 5 and 6. Retail phase 0 stores timer=20,
// advances to phase 1 and falls through to one countdown. The twentieth update
// advances phase 1->2 and writes the global wipe state to DONE.
static void test_test5_wipe_completes() {
	const int types[] = {5, 6};
	for (int type : types) {
		Hx_ResetWipe(640, 480);
		Hx_StartWipe(type, 0);
		int frames = 0;
		u32 state = 0;
		for (frames = 1; frames <= 30; ++frames) {
			state = Hx_UpdateWipe(1.0f);
			if (state == 3) break;
		}
		CHECK(state == 3, "type-5/6 Hx_Test5 reaches state DONE (3)");
		CHECK(frames == 20, "type-5/6 Hx_Test5 completes in EXACTLY 20 frames (li 0x14)");
	}
}

// A second wipe must work after a reset (state machine is re-entrant).
static void test_restart() {
	Hx_ResetWipe(640, 480);
	Hx_StartWipe(12, 0);
	bool done = false;
	for (int i = 0; i < 4000; ++i) {
		Hx_MovieStartSyncEx();
		if (Hx_UpdateWipe(1.0f) == 3) { done = true; break; }
	}
	CHECK(done, "wipe completes again after Hx_ResetWipe (re-entrant)");
}

int main() {
	Hx_ResetWipe(640, 480);
	Hx_StartWipe(10, 60);
	int type10_frames = 0;
	while (Hx_UpdateWipe(0.0f) != 3 && type10_frames < 100)
		++type10_frames;
	CHECK(type10_frames == 44, "type 10 completes after its four retail timers");

	test_get_wipe_type();
	test_mmark_wipe_completes();
	test_circle_wipe_completes();
	test_test5_wipe_completes();
	test_restart();
	if (g_fail) { std::fprintf(stderr, "hx_wipe_test: FAILURES\n"); return 1; }
	std::fprintf(stderr, "hx_wipe_test: all passed\n");
	return 0;
}
