// platform_stub.cpp — header compile-canary ONLY (not built into any target).
//
// The REAL platform bring-up is platform_impl.cpp (sb::platform::PlatformInit /
// Shutdown / PumpFrame). This file just #includes every seam header so they stay
// self-contained and parseable; it defines NO symbols (so it never conflicts with
// platform_impl.cpp).
//
// ARCHITECTURE NOTE: the per-subsystem *_seam.h headers below declare a parallel
// `sb::platform::<sub>::` C++ API that was an early design sketch. The shipping seams
// took the simpler, correct route instead: they DEFINE THE SDK C SYMBOLS the game
// actually links (OSCreateThread, DVDOpen, VIWaitForRetrace, PADRead, ...) directly,
// in <sub>_impl.cpp, matching the <dolphin/...> header signatures. So the namespaced
// declarations here are DESIGN NOTES (boot order, threading-model rationale, the
// GXColor-by-pointer caveat), not the implemented interface. Treat *_impl.cpp +
// dvd_disc.h / vi_present.h / pad_input.h as the source of truth.
#include "platform.h"
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
