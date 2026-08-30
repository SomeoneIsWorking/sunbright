#pragma once

#include <sunbright/native_render/semantic_2d_types.h>

#include <array>
#include <cstdint>

namespace sb::native_render {

enum class SolidRectangleSource : std::uint8_t {
    Unknown,
    Gc2dFillRect,
    J2dGrafContextFillBox,
};

struct TransformedS16RectangleLayout {
    std::int32_t x1 = 0;
    std::int32_t y1 = 0;
    std::int32_t x2 = 0;
    std::int32_t y2 = 0;
    Matrix3x4 transform{};
};

// Final above-GX rectangle values. Positions and colours use the same TL, TR, BL, BR corner
// convention as PictureCommand so both families can share one ordered semantic 2D stream.
struct SolidRectangleCommand {
    std::uint64_t instance = 0;
    SolidRectangleSource source = SolidRectangleSource::Unknown;
    std::array<Vec2, 4> positions{};
    std::array<Color, 4> corner{};
    ClipRect clip{};
};

struct SolidRectangleDraw {
    Canvas canvas{};
    SolidRectangleCommand rectangle{};
};

using SolidRectangleMesh = std::array<SemanticVertex, 6>;

[[nodiscard]] bool valid(const SolidRectangleCommand& rectangle) noexcept;
[[nodiscard]] bool valid(const SolidRectangleDraw& draw) noexcept;
// J2DGrafContext sends signed-16-bit positions through its loaded 3x4 matrix. Resolve that retail
// vertex contract once so native-layout and big-endian guest adapters cannot drift.
[[nodiscard]] bool resolve_transformed_s16_rectangle(const TransformedS16RectangleLayout& layout,
                                                     std::array<Vec2, 4>& positions) noexcept;
[[nodiscard]] SolidRectangleMesh make_mesh(const SolidRectangleCommand& rectangle) noexcept;

} // namespace sb::native_render
