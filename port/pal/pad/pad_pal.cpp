// PAD (GameCube controller) — Platform Abstraction Layer, NATIVE PC port.
//
// BRING-UP STUBS. These present a single, neutral (idle) controller on
// channel 0 and "no controller" on channels 1-3, so the engine links and
// runs deterministically before real input is wired. They are to be REPLACED
// by an SDL-driven implementation that polls real host gamepads/keyboard and
// fills the same PADStatus fields.
//
// Signatures must match <dolphin/pad.h> EXACTLY (declared `extern "C"`).
// A return-type or linkage mismatch silently breaks the link, so we include
// the real header and let the compiler enforce the prototypes.

#include <dolphin/pad.h>
#include <cstring>

extern "C" {

// PADInit / PADReset / PADRecalibrate: report success and "channel 0 enabled".
//
// PADInit/PADRecalibrate are declared returning BOOL (int): TRUE = ok.
// PADReset is declared `int PADReset(unsigned long mask)`; the real DOL returns
// the resetting-channel bitmask (PAD_CHANn_BIT). For a bring-up where only
// channel 0 exists, returning TRUE/non-zero is the sane "succeeded" value and
// is what every caller in the engine treats as success.

BOOL PADInit() {
	// Real PADInit returns TRUE on success (and enables channel 0).
	return TRUE;
}

int PADReset(unsigned long mask) {
	// Faithful DOL returns the bitmask of channels being (re)set; with only a
	// synthetic channel 0 there is nothing pending, so a non-zero "ok" value
	// is the neutral answer. Callers (JUTGamePad) ignore the value.
	(void)mask;
	return TRUE;
}

BOOL PADRecalibrate(u32 mask) {
	(void)mask;
	return TRUE;
}

// PADRead: fill 4 PADStatus with a neutral idle pad.
//
// Channel 0  -> connected, nothing pressed:
//     button=0, stick/substick centered (GC neutral for the signed s8 stick
//     fields is 0), triggers=0, analogA/B=0, err = PAD_ERR_NONE (0).
// Channels 1-3 -> err = PAD_ERR_NO_CONTROLLER (-1), all data zeroed.
//
// JUTGamePad::read() ignores PADRead's return value and dispatches purely on
// each channel's `err` field, so the field values above are what matters. We
// still return the faithful "error bitmask" (a channel's PAD_CHANn_BIT set
// when that channel has a non-NONE error) to mirror the DOL contract.
u32 PADRead(struct PADStatus* status) {
	if (status == nullptr) {
		return 0;
	}

	u32 err_bits = 0;
	for (int chan = 0; chan < PAD_MAX_CONTROLLERS; ++chan) {
		PADStatus& s = status[chan];
		std::memset(&s, 0, sizeof(PADStatus)); // button/sticks/triggers/analog = neutral 0

		if (chan == 0) {
			s.err = PAD_ERR_NONE; // connected, idle
		} else {
			s.err = (s8)PAD_ERR_NO_CONTROLLER;
			err_bits |= (PAD_CHAN0_BIT >> chan);
		}
	}
	return err_bits;
}

// PADClamp: real DOL clamps stick/substick magnitudes into the engine's dead
// zone / octagon. Our PADRead already produces a clamped (zeroed) neutral
// status, so this is a no-op until real input arrives.
void PADClamp(PADStatus* status) {
	(void)status;
}

// Rumble / spec / analog-mode: no-ops for bring-up.
void PADControlMotor(s32 chan, u32 command) {
	(void)chan;
	(void)command;
}

void PADControlAllMotors(const u32* commandArray) {
	(void)commandArray;
}

void PADSetSpec(u32 spec) {
	(void)spec;
}

void PADSetAnalogMode(u32 mode) {
	(void)mode;
}

} // extern "C"
