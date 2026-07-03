// engine.h — process-wide render-sink toggle for sms-boot.
//
// The problem: SMS_NATIVE_PLATFORM is a preprocessor gate, so a single build can
// only compile ONE render path. That prevents side-by-side "is our SDL3 pipeline
// wrong, or is the reference wrong?" comparisons in a single binary.
//
// This class holds a runtime choice for WHICH render sink the GX seam dispatches
// into. NATIVE_PC is today's SDL3 GPU renderer (the shipping product). GX_ORACLE
// routes GX calls into Dolphin's videovulkan backend as a reference-only sink
// used to A/B diagnose divergences. Fixes still land in NATIVE_PC — the oracle
// sink is a measurement instrument, never ship state.
//
// This toggle is the render-sink ONLY. It does NOT influence audio, threading,
// memory, or any other SMS_NATIVE_PLATFORM ifdef — those must remain compile-time
// because there's no alternative implementation for them (game code has to
// execute somewhere).
//
// Chosen at startup from SB_RENDER (or SUNBRIGHT_RENDER for parity with the
// rest of the diag env prefix). Default: NATIVE_PC.

#pragma once

namespace sb::engine {

enum class RenderMode {
    NATIVE_PC,   // sms-boot's SDL3 GPU renderer (default; the shipping product)
    GX_ORACLE,   // Dolphin videovulkan backend, reference-only diagnostic sink
};

// Initialize from environment (SB_RENDER=native|oracle, default native). Idempotent.
// Called once from boot.cpp before the game thread starts.
void init_from_env();

// Current mode. Reads a plain global — cheap enough to call per-draw. Set once
// at startup, never mutated after init.
RenderMode mode();

// Human-readable name of the current mode, for logs / probe.
const char* mode_name();

} // namespace sb::engine
