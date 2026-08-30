#pragma once

#include <array>
#include <cstdint>

namespace sb::native_render {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
    bool operator==(const Vec2&) const = default;
};

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
    bool operator==(const Color&) const = default;
};

[[nodiscard]] Color color_from_rgba8(std::uint32_t rgba) noexcept;

struct ClipRect {
    bool enabled = false;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct PixelRect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool operator==(const PixelRect&) const = default;
};

// One semantic screen's logical coordinate system and its physical placement inside the native
// target. A game frame may contain several distinct canvases, so this travels with each draw.
struct Canvas {
    Vec2 origin{};
    Vec2 extent{};
    PixelRect viewport{};
    bool operator==(const Canvas&) const = default;
};

struct SemanticVertex {
    Vec2 position{};
    Vec2 uv{};
    Color color{};
    bool operator==(const SemanticVertex&) const = default;
};

struct Matrix3x4 {
    std::array<float, 12> value{};
};

[[nodiscard]] bool valid(const Canvas& canvas) noexcept;
[[nodiscard]] bool valid(const Matrix3x4& matrix) noexcept;
[[nodiscard]] Matrix3x4 concatenate_transform(const Matrix3x4& first,
                                              const Matrix3x4& second) noexcept;
[[nodiscard]] Vec2 transform_point(const Matrix3x4& matrix, float x, float y) noexcept;
[[nodiscard]] bool resolve_scissor(const Canvas& canvas, const ClipRect& clip,
                                   std::uint32_t targetWidth, std::uint32_t targetHeight,
                                   PixelRect& scissor) noexcept;

} // namespace sb::native_render
