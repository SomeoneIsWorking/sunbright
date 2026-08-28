#pragma once

#include <sunbright/native_render/picture.h>

#include <array>
#include <cstddef>

namespace sb::native_render {

struct PictureContext {
    Canvas canvas{};
    bool clipEnabled = false;
};

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
