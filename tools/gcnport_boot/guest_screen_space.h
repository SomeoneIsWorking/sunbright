// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

#include <sunbright/native_render/semantic_2d_types.h>
#include <sunbright/title_adapter/guest_projection.h>

namespace sunbright::gcnport_boot {

// The screen GMSE01's un-owned 2D lands in.
//
// Most of the title's 2D is a `J2DPane` under a `J2DGrafContext`, which states its own logical
// screen and viewport; `GuestJ2dContextProbe` follows those. `TSMSFader` is not one of them.
// `TApplication::gameLoop` sets an orthographic projection with `C_MTXOrtho` and a region with
// `GXSetViewport`, then hands the fader a rectangle and nothing else, so the screen its quad is in
// exists only as GX state. Both values are sticky -- they govern every draw until something
// replaces them -- so "the screen in force" is exactly "the last pair set", which is what this
// holds.
//
// It is fed rather than hooked: the two probes already on `GXSetProjection` and `GXSetViewport`
// pass their values here, so there is one hook per entry and one place that knows what a screen is.
class GuestScreenSpace {
  public:
    void set_orthographic(const sb::title_adapter::GuestOrthographicScreen& screen) noexcept;
    // A perspective projection is a 3D camera, not a screen. It replaces the orthographic one
    // rather than leaving it standing, because a 2D quad submitted under it would be placed by a
    // screen the hardware is no longer using.
    void set_perspective() noexcept;
    void set_viewport(std::int32_t x, std::int32_t y, std::int32_t width,
                      std::int32_t height) noexcept;

    // The canvas in force, or null when no orthographic projection and viewport pair has been set.
    [[nodiscard]] const sb::native_render::Canvas* current() const noexcept {
        return has_ ? &canvas_ : nullptr;
    }

    void report() const;

  private:
    void recompute() noexcept;

    sb::title_adapter::GuestOrthographicScreen screen_{};
    sb::native_render::PixelRect viewport_{};
    bool hasScreen_ = false;
    bool hasViewport_ = false;
    sb::native_render::Canvas canvas_{};
    bool has_ = false;

    std::uint64_t orthographicProjections_ = 0;
    std::uint64_t perspectiveProjections_ = 0;
    std::uint64_t viewports_ = 0;
    // A screen and viewport pair that is not a canvas this renderer can draw into -- an inverted
    // vertical axis, or a region with no area. Counted rather than clamped: a quad placed in a
    // screen the title did not state is a defect that looks like a plausible frame.
    std::uint64_t invalidCanvas_ = 0;
    std::uint64_t changes_ = 0;
};

} // namespace sunbright::gcnport_boot
