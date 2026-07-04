// engine.h — process-wide render-path selector for sms-boot.
//
// sms-boot has two render paths (post-2026-07-04 rework):
//   PC_NATIVE   — reference/sms decomp emits PC-native SDL3-GPU calls directly.
//                 Deferred until the GC-faithful path reaches pixel parity.
//   GC_FAITHFUL — reference/sms decomp emits its original GameCube GX SDK calls;
//                 they run through Aurora (encounter/aurora) onto WebGPU/Dawn.
//                 This is the in-process oracle chasing Dolphin-GX parity, and
//                 the default until Path A pixel parity closes.
//
// Selection is compile-time (-DSB_RENDER=gc|pc). The enum here is a runtime
// witness for logging/probe endpoints — the actual path is baked into which
// backend headers reference/sms sees at include time.

#pragma once

namespace sb::engine {

enum class RenderMode {
    PC_NATIVE,    // reference/sms -> SDL3-GPU direct
    GC_FAITHFUL,  // reference/sms -> Aurora <dolphin/gx.h> -> WebGPU/Dawn
};

// Initialize from environment (kept for probe/log parity; the real path is
// compile-time). Idempotent. Called once from boot.cpp.
void init_from_env();

RenderMode mode();
const char* mode_name();

} // namespace sb::engine
