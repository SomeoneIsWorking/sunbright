#pragma once

#include <utility>

// Render-pass bindings do not survive SDL_EndGPURenderPass. Route every pass creation through this
// helper so an EFB-copy resume cannot issue its first draw against an unbound vertex input.
template <typename Begin, typename Bind>
[[nodiscard]] auto sbr_native_begin_render_pass(Begin&& begin, Bind&& bind, bool hasVertices) {
    auto pass = std::forward<Begin>(begin)();
    if (hasVertices)
        std::forward<Bind>(bind)(pass);
    return pass;
}
