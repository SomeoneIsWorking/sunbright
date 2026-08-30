#pragma once

#include <sunbright/native_render/picture.h>
#include <sunbright/native_render/solid_rectangle.h>

#include <array>
#include <cstdint>

namespace sb::native_render {

enum class WindowTextureRole : std::uint8_t {
    Contents,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

struct WindowTextureExtent {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Inputs owned by J2DWindow. The shared resolver is the sole implementation of the retail window
// layout: both game runtimes adapt their distinct object layouts into these values.
struct WindowLayout {
    std::int32_t outerWidth = 0;
    std::int32_t outerHeight = 0;
    std::int32_t minimumWidth = 1;
    std::int32_t minimumHeight = 1;
    std::int32_t contentsX1 = 0;
    std::int32_t contentsY1 = 0;
    std::int32_t contentsX2 = 0;
    std::int32_t contentsY2 = 0;
    std::uint32_t mirror = 0;
    bool hasFrame = false;
    bool hasContentsTexture = false;
    std::array<WindowTextureExtent, 4> frameTextures{};
    WindowTextureExtent contentsTexture{};
    Matrix3x4 parentTransform{};
    Matrix3x4 globalTransform{};
};

struct WindowTexturedPart {
    WindowTextureRole texture = WindowTextureRole::Contents;
    std::array<Vec2, 4> positions{};
    std::array<Vec2, 4> uv{};
    bool visible = false;
};

struct WindowGeometry {
    std::array<Vec2, 4> contentsPositions{};
    bool contentsVisible = false;
    WindowTexturedPart contentsTexture{};
    std::array<WindowTexturedPart, 8> frame{};
};

enum class WindowLayoutResult : std::uint8_t { Visible, Culled, Invalid };

[[nodiscard]] bool window_size_is_culled(const WindowLayout& layout) noexcept;

// Resolves the exact J2DWindow::draw_private geometry after its parent/global transform. Position
// inputs narrow to signed 16-bit just as the retail vertex writer does. A too-small outer rectangle
// is an intentional retail no-op (Culled), while malformed texture state is Invalid.
[[nodiscard]] WindowLayoutResult resolve_window_layout(const WindowLayout& layout,
                                                       WindowGeometry& geometry) noexcept;

// Builds the existing generic semantic primitives for a resolved window part. The renderer needs
// no window-specific shader: windows are an ordered composition of one gradient rectangle and
// one-texture picture quads.
[[nodiscard]] bool make_window_contents_command(const WindowGeometry& geometry,
                                                std::uint64_t instance,
                                                const std::array<Color, 4>& colors,
                                                SolidRectangleCommand& command) noexcept;
[[nodiscard]] bool make_window_picture_command(const WindowTexturedPart& part,
                                               std::uint64_t instance,
                                               const PictureTexture& texture, float opacity,
                                               Color black, Color white,
                                               PictureCommand& command) noexcept;

} // namespace sb::native_render
