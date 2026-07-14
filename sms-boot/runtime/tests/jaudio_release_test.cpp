// jaudio_release_test.cpp — verify-first unit test locking the JAudio
// handle-pool release / double-release invariants fixed in reference/sms
// fdce9d18 (debug_journal/2026-07-10_audio_double_release_retail_audit.md).
//
// This links the REAL JAIBasic/JAISound code (compiled into the sms-native
// static library exactly as sms-boot uses it, SMS_NATIVE_PLATFORM=1) rather
// than a hand copy — JAIBasic::releaseControllerHandle and
// JAIBasic::stopSeq are exercised directly. JAIData has a declared-but-never-
// defined default constructor (dead in the real game too: nothing calls
// `new JAIData()`), so a JAIData instance can't be value-constructed; the
// fixture instead reinterprets a zeroed byte buffer as JAIData and only
// wires the handful of fields (unk210 seq-handle link buffer, unk180
// update-slot table) that stopSeq/releaseControllerHandle actually touch.
// JAIBasic itself has the same dead-destructor problem, so the instance is
// heap-allocated and intentionally never deleted (single short-lived test
// process).
//
// No GPU / no ROM — pure list algebra.

#include <JSystem/JAudio/JAInterface/JAIBasic.hpp>
#include <JSystem/JAudio/JAInterface/JAISound.hpp>
#include <JSystem/JAudio/JAInterface/JAIData.hpp>
#include <JSystem/JAudio/JAInterface/JAIParameters.hpp>
#include <JSystem/JAudio/JAInterface/JAISystemInterface.hpp>

#include <cstdio>
#include <cstring>

// This narrow test links only sms-native/sms-gd/sms-assets (no sms-boot
// runtime, no aurora::audio) and never drives a video/audio frame, so it
// never reaches JASystem::DSPBuf::process's DsyncFrame2 call in practice --
// but JASDSPBuf.cpp.o (part of sms-native) still references the symbol at
// link time. The real body lives in sms-boot/runtime/jas_kernel_native.cpp
// (audio milestone 1, docs/audio_native_mixer_plan.md); pulling that whole
// TU in here would drag in gpMSound -> Application.cpp -> the full
// TApplication/MarDirector/ScrnFader chain this test intentionally excludes.
// A local link-satisfying stub is correct here: it's test-harness scaffold,
// not a runtime seam a real boot could hit silently.
void DsyncFrame2(u32, uintptr_t, uintptr_t) { }

static int g_fail = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); g_fail = 1; } \
	else         { std::fprintf(stderr, "ok:   %s\n", msg); } \
} while (0)

// Zeroed fixture standing in for a JAIData instance. JAIData::JAIData() is
// declared in JAIData.hpp but never defined anywhere in reference/sms (no
// caller either -- dead in the real game), so `JAIData d;` is a link error.
// Every JAIBasic method exercised here (stopSeq, releaseControllerHandle)
// only ever touches unk210/unk180 through the `unk0` (JAIData*) pointer, so
// zero-initialized raw storage reinterpreted as JAIData, with just those two
// fields wired up, is a faithful enough stand-in for this test's purposes.
struct JAIDataFixture {
	alignas(JAIData) unsigned char storage[sizeof(JAIData)];
	JAIData* get() { return reinterpret_cast<JAIData*>(storage); }
	JAIDataFixture() { std::memset(storage, 0, sizeof(storage)); }
};

// getSwBit() (via JAISound::getSwBit -> JAIBasic::getSoundSwBit) reads
// *(JAISoundInfo*)unk3C and byte-swaps it (SMS_NATIVE_PLATFORM). unk0=0
// keeps `getSwBit() & 1` false for every fixture sound, skipping stopSeq's
// unrelated cross-track fade-out loop (which needs a fully populated
// unk180 track table we don't build here).
static JAISoundInfo g_quietSwBit = { 0, 0, 0, 0.0f, 0, 0 };

int main()
{
	// One JAIBasic sets JAISound::interPointer (used by JAISound::getSwBit)
	// and JAIBasic::basic. ~JAIBasic() is likewise declared-but-undefined
	// (dead code, no caller in the real game either) -- heap-allocate and
	// leak it rather than let a scope-exit destructor call fail to link.
	JAIBasic* jai = new JAIBasic();

	JAIDataFixture dataFixture;
	JAIData* data = dataFixture.get();
	jai->unk0     = data;

	JAISeqUpdateData updateSlots[1];
	std::memset(updateSlots, 0, sizeof(updateSlots));
	data->unk180 = updateSlots;

	// -------------------------------------------------------------------
	// Test 1: normal release via the REAL stopSeq() non-early-out path --
	// active-list handle released once lands on the free list, the seq
	// track slot is cleared, and unk34 (the game-side back-pointer cache)
	// is nulled.
	// -------------------------------------------------------------------
	{
		JAISeqParameter seqParam;
		std::memset(&seqParam, 0, sizeof(seqParam));
		data->unk1C0 = &seqParam; // seq-parameter "active" list head == seqParam
		data->unk1BC = nullptr;   // seq-parameter free-list tail, empty

		JAISound sound;
		sound.unk0  = 0;               // updateSlots index
		sound.unk1  = 1;               // < 3: skip releaseAutoHeapPointer branch
		sound.unk2C = nullptr;         // fresh handle, no stale neighbor
		sound.unk30 = nullptr;
		JAISound* backPointerSlot = &sound;
		sound.unk34 = &backPointerSlot; // game-side cache pointing at itself
		sound.unk38 = &seqParam;        // getSeqParameter() != nullptr
		sound.unk3C = &g_quietSwBit;

		data->unk210.unk4 = &sound; // this handle is the seq "in-use" list head
		data->unk210.unk0 = nullptr;
		updateSlots[0].unk48 = &sound;

		jai->stopSeq(&sound);

		CHECK(sound.unk38 == nullptr, "T1: released handle's getSeqParameter() == nullptr");
		CHECK(sound.unk34 == nullptr, "T1: released handle's unk34 back-pointer cleared");
		CHECK(sound.unk1 == 0, "T1: released handle's unk1 state reset to 0");
		CHECK(data->unk210.unk0 == &sound, "T1: handle lands on the seq free list (unk210.unk0)");
		CHECK(data->unk210.unk4 == nullptr, "T1: handle removed from the seq in-use list (unk210.unk4)");
		CHECK(updateSlots[0].unk48 == nullptr, "T1: seq track slot cleared");
		CHECK(data->unk1C0 == nullptr, "T1: seq-parameter active-list head cleared");
		CHECK(data->unk1BC == &seqParam, "T1: seq-parameter moved onto its own free list");
		CHECK(backPointerSlot == &sound, "T1: unk34's OLD target slot untouched (stopSeq nulls the unk34 field itself, not *unk34 -- that's clearMainSoundPPointer's job, called separately by every real release site)");
	}

	// -------------------------------------------------------------------
	// Test 2: double release via stopSeq's already-released early-out
	// (fdce9d18). A handle already sitting on the free list (unk38==null)
	// gets stopSeq()'d again -- e.g. a stale game-side cache left dangling
	// by one of retail's own un-guarded release paths (checkEntriedSeq /
	// checkReadSeq failure branches; see the retail audit doc). The FIX
	// makes the second call a pure list no-op. Reverting fdce9d18 restores
	// the old behavior of re-releasing through releaseControllerHandle,
	// which self-loops the free list and corrupts a sibling handle's link
	// via the stale unk2C pointer -- this test must go red under that
	// revert (verified manually, see report).
	// -------------------------------------------------------------------
	{
		JAISound sibling;
		std::memset(&sibling, 0, sizeof(sibling));
		JAISound canarySink;
		std::memset(&canarySink, 0, sizeof(canarySink));
		sibling.unk30 = &canarySink; // sentinel: must survive untouched

		JAISound sound;
		std::memset(&sound, 0, sizeof(sound));
		sound.unk0  = 0;
		sound.unk3C = &g_quietSwBit;
		sound.unk2C = &sibling; // stale neighbor link left over from a prior list
		sound.unk30 = nullptr;

		// Fabricate "already released once" by driving the real
		// releaseControllerHandle on the SAME buffer (unk210) stopSeq's
		// early-out (pre-fix) would re-enter -- this is the exact
		// production function every ordinary release path (including
		// stopSeq's own non-early-out tail) calls.
		data->unk210.unk4 = &sound; // mark as in-use list head so the first
		data->unk210.unk0 = nullptr; // release takes the simple head-removal branch
		jai->releaseControllerHandle(&data->unk210, &sound);

		CHECK(sound.unk38 == nullptr, "T2 setup: handle is in the already-released state");
		CHECK(data->unk210.unk0 == &sound, "T2 setup: handle sits on the free list");
		JAISound* freeListHeadAfterFirstRelease = data->unk210.unk0;
		JAISound* soundNextAfterFirstRelease    = sound.unk30;

		// Second stop: stopSeq's SMS_NATIVE_PLATFORM early-out branch.
		updateSlots[0].unk48 = &sound; // (only touched if the old re-release path runs)
		jai->stopSeq(&sound);

		CHECK(sound.unk34 == nullptr, "T2: second stop still clears unk34 (list no-op, not a crash)");
		CHECK(sound.unk1 == 0, "T2: second stop still resets unk1 (list no-op, not a crash)");
		CHECK(sound.unk30 != &sound, "T2: free list NOT self-looped by the second stop");
        CHECK(sound.unk30 == soundNextAfterFirstRelease,
              "T2: handle's own free-list link (unk30) unchanged by the second stop");
		CHECK(data->unk210.unk0 == freeListHeadAfterFirstRelease,
		      "T2: free-list head (unk210.unk0) unchanged by the second stop");
		CHECK(sibling.unk30 == &canarySink,
		      "T2: sibling handle's link (unk30) untouched by the second stop -- no cross-handle corruption");
	}

	// -------------------------------------------------------------------
	// Test 3: releaseControllerHandle with a null unk2C (the stream-path
	// double-release shape kept guarded in releaseControllerHandle itself,
	// same commit). A handle that isn't the list head and never had its
	// unk2C wired to a real neighbor must not crash, and the rest of the
	// list must stay consistent.
	// -------------------------------------------------------------------
	{
		JAISound listHead;
		std::memset(&listHead, 0, sizeof(listHead));
		JAISound headNextSentinel;
		std::memset(&headNextSentinel, 0, sizeof(headNextSentinel));
		listHead.unk30 = &headNextSentinel; // canary: must survive untouched

		JAILinkBuffer buffer;
		buffer.unk0 = nullptr; // free list empty
		buffer.unk4 = &listHead; // in-use list head is a DIFFERENT handle
		buffer.unk8 = nullptr;

		JAISound target;
		std::memset(&target, 0, sizeof(target));
		target.unk2C = nullptr; // never spliced in -- the stream-path shape
		target.unk30 = nullptr;

		jai->releaseControllerHandle(&buffer, &target); // must not crash

		CHECK(target.unk38 == nullptr, "T3: released handle's unk38 cleared");
		CHECK(target.unk34 == nullptr, "T3: released handle's unk34 cleared");
		CHECK(buffer.unk0 == &target, "T3: handle prepended onto the free list");
		CHECK(buffer.unk4 == &listHead, "T3: in-use list head untouched (target wasn't a member)");
		CHECK(listHead.unk30 == &headNextSentinel, "T3: unrelated list-head handle's link untouched (null-unk2C guard held)");
	}

	if (g_fail) {
		std::fprintf(stderr, "JAudio handle-pool release/double-release test: FAILED\n");
		return 1;
	}
	std::fprintf(stderr, "JAudio handle-pool release/double-release test: PASSED\n");
	return 0;
}
