// platform.h — top-level native platform bring-up / teardown.
//
// Replaces the GC __start -> OSInit -> (subsystem inits) -> game-main sequence
// with a native equivalent. The game's main() (reference/sms/src/main.cpp) is
// reached AFTER PlatformInit() has stood up every seam below, in dependency
// order. See README.md "Boot order".
//
// This is the ONLY file phase-2 integration glue should need to include to wire
// the platform up; individual subsystems are in *_seam.h.
#pragma once
#include "platform_types.h"

namespace sb::platform {

// Stand up all seams in dependency order (clock/os -> dvd/fs -> audio/video/input).
// Returns false if a mandatory subsystem fails (e.g. no disc image, no audio device).
bool PlatformInit(int argc, char** argv);

// Tear everything down (reverse order). Safe to call once after PlatformInit.
void PlatformShutdown();

// Per-frame host pump: called once per presented frame from the VI/present path.
// Pumps the host event loop (window, input) and advances host-clock-driven seams.
void PlatformPumpFrame();

} // namespace sb::platform
