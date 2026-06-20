// vi_present.h — present/pacing hooks for the VI seam (native/platform/vi_impl.cpp).
//
// The VI seam owns the 60 Hz retrace heartbeat (VIWaitForRetrace + retrace callbacks)
// and the current-framebuffer bookkeeping. The ACTUAL present (pushing a frame to a
// host swapchain) is the integration layer's job — it queries the seam for the
// last-set framebuffer and pacing, and the GX/VI integration wires a real surface.
#pragma once
#include <dolphin/types.h>

extern "C" {
// Enable/disable host-clock pacing in VIWaitForRetrace (default ON). Tests turn it
// OFF so they don't sleep ~16ms per retrace.
void sb_vi_set_pacing(bool on);
// The framebuffer last handed to VISetNextFrameBuffer (for the integration present).
void* sb_vi_current_framebuffer(void);
// Install a present hook called once per retrace (after the post-retrace callback)
// with the current framebuffer — the integration layer pushes it to the swapchain.
typedef void (*ViPresentFn)(void* framebuffer, void* user);
void sb_vi_set_present_hook(ViPresentFn fn, void* user);
}
