// platform_stub.cpp — compile-canary for the seam headers.
//
// This file exists ONLY to prove every seam header is self-contained and compiles
// (each is #included below). It is NOT the implementation — phase-2 replaces the
// per-subsystem TODO bodies with real native code in dedicated translation units
// (os_seam.cpp, gx_seam.cpp, ...). The PlatformInit/Shutdown/PumpFrame bodies here
// are minimal stubs returning sensible defaults so the platform layer links during
// scaffolding; they call into the (not-yet-implemented) seams' Init in boot order.
#include "platform.h"

// Include every seam header so a build of this TU type-checks the whole surface.
#include "platform_types.h"
#include "os_seam.h"
#include "mtx_seam.h"
#include "gx_seam.h"
#include "dvd_seam.h"
#include "card_seam.h"
#include "audio_seam.h"
#include "vi_seam.h"
#include "pad_seam.h"
#include "thp_seam.h"
#include "exi_seam.h"

namespace sb::platform {

bool PlatformInit(int /*argc*/, char** /*argv*/) {
    // Boot order (see README.md "Boot order"). Real bodies land in phase-2; for now
    // this is the call sequence the integration glue will use.
    // os::Init(); dvd::Init(); card::Init(); audio::Init();
    // vi::Init(); gx::Init(); pad::Init(); thp::Init();
    return true;   // sensible default while seams are unimplemented
}

void PlatformShutdown() {
    // Reverse order; audio::Shutdown() etc. land in phase-2.
}

void PlatformPumpFrame() {
    // Pump host events + advance host-clock-driven seams (phase-2).
}

} // namespace sb::platform
