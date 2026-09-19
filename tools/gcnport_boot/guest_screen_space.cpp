// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_screen_space.h"

#include <cstdio>

namespace sunbright::gcnport_boot {

void GuestScreenSpace::set_orthographic(
    const sb::title_adapter::GuestOrthographicScreen& screen) noexcept {
    orthographicProjections_ += 1;
    screen_ = screen;
    hasScreen_ = true;
    recompute();
}

void GuestScreenSpace::set_perspective() noexcept {
    perspectiveProjections_ += 1;
    hasScreen_ = false;
    recompute();
}

void GuestScreenSpace::set_viewport(std::int32_t x, std::int32_t y, std::int32_t width,
                                    std::int32_t height) noexcept {
    viewports_ += 1;
    viewport_ = {x, y, static_cast<std::uint32_t>(width < 0 ? 0 : width),
                 static_cast<std::uint32_t>(height < 0 ? 0 : height)};
    hasViewport_ = true;
    recompute();
}

void GuestScreenSpace::recompute() noexcept {
    if (!hasScreen_ || !hasViewport_) {
        has_ = false;
        return;
    }
    const sb::native_render::Canvas canvas{
        {screen_.left, screen_.top},
        {screen_.right - screen_.left, screen_.bottom - screen_.top},
        viewport_};
    if (!sb::native_render::valid(canvas)) {
        invalidCanvas_ += 1;
        has_ = false;
        return;
    }
    if (!has_ || canvas != canvas_) {
        changes_ += 1;
    }
    canvas_ = canvas;
    has_ = true;
}

void GuestScreenSpace::report() const {
    std::printf("gmse01_boot: guest screen space: %llu orthographic and %llu perspective "
                "projection(s), %llu viewport(s), %llu distinct screen(s)\n",
                static_cast<unsigned long long>(orthographicProjections_),
                static_cast<unsigned long long>(perspectiveProjections_),
                static_cast<unsigned long long>(viewports_),
                static_cast<unsigned long long>(changes_));
    if (invalidCanvas_ != 0) {
        std::printf("gmse01_boot:   %llu pair(s) were not a canvas this renderer can draw into\n",
                    static_cast<unsigned long long>(invalidCanvas_));
    }
    if (has_) {
        std::printf("gmse01_boot:   in force: %gx%g at (%g, %g) into viewport %ux%u at (%d, %d)\n",
                    static_cast<double>(canvas_.extent.x), static_cast<double>(canvas_.extent.y),
                    static_cast<double>(canvas_.origin.x), static_cast<double>(canvas_.origin.y),
                    canvas_.viewport.width, canvas_.viewport.height, canvas_.viewport.x,
                    canvas_.viewport.y);
    } else {
        std::printf("gmse01_boot:   no orthographic screen is in force; nothing outside a J2D "
                    "graphics context had anywhere to be drawn\n");
    }
}

} // namespace sunbright::gcnport_boot
