#pragma once

#include <sunbright/native_render/semantic_2d_types.h>

#include <array>
#include <cstdint>

namespace sb::native_render {

// Final above-GX rectangle values. Positions and colours use the same TL, TR, BL, BR corner
// convention as PictureCommand so both families can share one ordered semantic 2D stream.
struct SolidRectangleCommand {
    std::uint64_t instance = 0;
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
[[nodiscard]] SolidRectangleMesh make_mesh(const SolidRectangleCommand& rectangle) noexcept;

} // namespace sb::native_render
