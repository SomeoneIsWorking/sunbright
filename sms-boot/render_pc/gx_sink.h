// gx_sink.h — the render-sink boundary for sms-boot.
//
// The game code (reference/sms, native/platform/gx_*.cpp) captures GX state,
// immediate-mode geometry, and per-batch TEV/material snapshots into CPU-side
// buffers each frame. At present time, ONE of two sinks consumes those buffers
// and produces pixels:
//
//   NATIVE_PC  → sb::render::native_present_frame(fb, user)
//                  — sms-boot's SDL3 GPU renderer, the ship product.
//   GX_ORACLE  → sb::render::oracle_present_frame(fb, user)
//                  — routes the same captured state through Dolphin's
//                    videovulkan backend, matching build/sunbright's oracle
//                    pipeline. Reference-only diagnostic sink; never ship state.
//
// The dispatch itself lives in sms_boot_present.cpp's VI-present hook: it reads
// sb::engine::mode() and forwards to the matching function. Neither sink is
// virtual — the choice is per-process at startup — so this is a plain function-
// pointer boundary, not a vtable.

#pragma once

namespace sb::render {

// Signature matches the sb_vi_set_present_hook API (native/platform/vi_present.h).
using PresentFn = void (*)(void* framebuffer, void* user);

// The two sinks. Prototypes here; bodies in native_present.cpp / oracle_present.cpp
// (currently native_present.cpp is the pre-existing sms_boot_present.cpp body).
void native_present_frame(void* framebuffer, void* user);
void oracle_present_frame(void* framebuffer, void* user);

} // namespace sb::render
