#pragma once

#include "native_render.h"

struct NativeEfbCopyClearDraw {
    SbrVertex vertices[3]{};
    SbrDepthState state{};
    SbrTexture textures[8]{};
    SbrTevState tev{};
};

// Build the backend-independent draw that performs the post-copy PE clear. Returns false when the
// clipped plan has no clear operation, including the Hx_Test5 row whose source starts offscreen.
[[nodiscard]] bool sbr_native_efb_copy_clear_draw(const NativeEfbCopyPlan& plan,
                                                  NativeEfbCopyClearDraw& draw) noexcept;
