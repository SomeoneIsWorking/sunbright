#pragma once

#include <sunbright/native_render/picture.h>

#include <array>
#include <cstddef>

namespace sb::native_render {

struct PictureContext {
    Canvas canvas{};
    ClipRect scissor{};
    bool clipEnabled = false;
};

// J2DGrafContext::setScissor normalizes its integer bounds, shifts the top edge up one pixel, and
// clamps to the retail 1024x1000 guard rectangle before programming GX. Preserve that high-level
// J2D contract as a target-pixel clip; a zero extent is a legitimate ordered no-op.
[[nodiscard]] ClipRect j2d_target_scissor(std::int32_t x1, std::int32_t y1, std::int32_t x2,
                                          std::int32_t y2) noexcept;

// J2DScreen traversal is nested but allocation-free. Each runtime owns its own instance and copies
// graph values on entry, so no pointer to a stack-local J2DOrthoGraph crosses the semantic seam.
class PictureContextStack {
  public:
    static constexpr std::size_t kCapacity = 16;

    [[nodiscard]] bool push(PictureContext context) noexcept;
    [[nodiscard]] bool pop() noexcept;
    [[nodiscard]] const PictureContext* current() const noexcept;
    [[nodiscard]] std::size_t depth() const noexcept;

  private:
    std::array<PictureContext, kCapacity> contexts_{};
    std::size_t depth_ = 0;
};

} // namespace sb::native_render
