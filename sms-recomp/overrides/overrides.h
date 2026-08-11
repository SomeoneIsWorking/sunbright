// overrides.h — native replacements for recompiled guest functions.
//
// This is the "recomp WITH overrides" seam: the recompiled DOL is the whole game, and
// individual guest functions are swapped for native C++ ones. Two reasons a function
// gets overridden, and only these two:
//
//   1. It talks to hardware this port does not have and never will (DSP, DI, SI). The
//      standalone port drives audio/video/input through the host, so emulating the
//      register-level handshake would be building an emulator we then throw away.
//   2. It is a known-correct native replacement (faster, or fixes a host-layout hazard).
//
// An override is NOT a way to skip a function that merely misbehaves — that is a recomp
// bug and must be fixed in the recompiler. Every override carries a reason string and
// announces itself on first call, so a silent no-op can never masquerade as working code
// (the no-silent-stubs rule).

#pragma once

#include "cpu_state.h"

// Look up a native override for a guest address. Returns nullptr when the address should
// run its recompiled body, which is the case for all but a handful of functions.
void (*override_lookup(u32 address))(CPUState&);

// Is there one, without announcing it? Asking is not executing, and only override_lookup logs the
// "-> native" line that says a function was actually replaced at run time.
bool override_exists(u32 address);

// Register at static-init time via the macro below.
void override_register(u32 address, void (*fn)(CPUState&), const char* symbol,
                       const char* reason);

#define SB_OVERRIDE(addr, fn, symbol, reason)                                          \
    namespace {                                                                        \
    struct fn##_reg {                                                                  \
        fn##_reg() { override_register((addr), &fn, (symbol), (reason)); }             \
    } fn##_reg_instance;                                                               \
    }
