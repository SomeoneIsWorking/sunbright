// oracle_present.cpp — GX_ORACLE render sink for sms-boot.
//
// When sb::engine::mode() == GX_ORACLE, sms_boot_present.cpp's VI-present hook
// forwards to sb_oracle_present_frame() here instead of running the SDL3 GPU
// pipeline. The sink is expected to consume the SAME captured GX state /
// geometry the NATIVE_PC sink does (via sb_boot_capture_tev_take,
// sb_imm_take_batch, gx_state.h globals), route it through Dolphin's
// videovulkan backend, and push the resulting frame to the window via
// sb::gxsdl::inject_cpu_frame.
//
// STATUS (Path C step 3): plumbing stub only. The sink currently paints a
// distinctive magenta+diagonal-stripe pattern into the window frame so that
// running `SB_RENDER=oracle build/native/sms-boot` visibly differs from the
// NATIVE_PC sink — proving end-to-end that the runtime toggle takes effect.
// Real Dolphin VideoCommon initialisation + GX-call routing lands in step 4.
//
// This file is ONLY compiled+linked when Dolphin's videovulkan target is
// visible in the CMake scope (see native/CMakeLists.txt: `if(TARGET
// videovulkan)`). A standalone `native/`-only build omits it, and
// sms_boot_present.cpp's dispatch falls back to NATIVE_PC.

#include "gx_sdlgpu.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

// Once-per-process init logging so the user sees the oracle sink is active.
bool s_announced = false;

// Paint pattern helper — distinctive so a visual diff vs NATIVE_PC is obvious.
void paint_stub(std::vector<uint8_t>& rgba, int w, int h) {
    // Magenta base + white diagonal stripes every 32 px. Frame counter tints the
    // top strip so a static screen still animates (proves per-frame invocation).
    static int s_tick = 0;
    ++s_tick;
    const uint8_t tint = (uint8_t)((s_tick * 7) & 0xff);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = rgba.data() + ((size_t)y * w + x) * 4;
            const bool stripe = ((x + y) / 32) & 1;
            p[0] = stripe ? 0xff : 0xff;       // R
            p[1] = stripe ? 0xff : 0x20;       // G — magenta base, white stripe
            p[2] = stripe ? 0xff : 0xff;       // B
            p[3] = 0xff;                        // A
            if (y < 16) p[0] = tint;            // top strip: animating tick
        }
    }
}

} // namespace

extern "C" void sb_oracle_present_frame(void* /*framebuffer*/, void* /*user*/) {
    if (!s_announced) {
        s_announced = true;
        std::fprintf(stderr, "[oracle] sink active — stub magenta pattern until Dolphin videovulkan wire-up lands\n");
    }

    int w = 0, h = 0;
    sb::gxsdl::backbuffer_size(&w, &h);
    if (w <= 0 || h <= 0) return;  // window/backbuffer not up yet

    static std::vector<uint8_t> s_buf;
    const size_t need = (size_t)w * h * 4;
    if (s_buf.size() != need) s_buf.assign(need, 0);

    paint_stub(s_buf, w, h);

    sb::gxsdl::inject_cpu_frame(s_buf.data(), w, h);

    // NOTE: the NATIVE_PC path also drains sb_boot_capture_tev_take + the imm
    // capture buffers every present (otherwise they grow unbounded). This stub
    // does NOT drain — the buffers stay untouched under GX_ORACLE. Once the
    // Dolphin sink actually consumes captured state we'll drain here; for now,
    // running under SB_RENDER=oracle for long sessions will leak the capture
    // buffers, so treat this stub as a smoke-test-only mode.
}
