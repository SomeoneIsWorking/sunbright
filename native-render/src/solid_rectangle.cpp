#include <sunbright/native_render/solid_rectangle.h>

#include <algorithm>
#include <cmath>

namespace sb::native_render {
namespace {

bool finite(Vec2 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool finite(Color value) noexcept {
    return std::isfinite(value.r) && std::isfinite(value.g) && std::isfinite(value.b) &&
           std::isfinite(value.a);
}

float area2(Vec2 a, Vec2 b, Vec2 c) noexcept {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

float narrow_s16(std::int32_t value) noexcept {
    const std::uint32_t low = static_cast<std::uint32_t>(value) & 0xffffU;
    return static_cast<float>(low >= 0x8000U ? static_cast<std::int32_t>(low) - 0x10000
                                             : static_cast<std::int32_t>(low));
}

} // namespace

bool valid(const SolidRectangleCommand& rectangle) noexcept {
    if (rectangle.instance == 0 || rectangle.source == SolidRectangleSource::Unknown) {
        return false;
    }
    for (std::size_t index = 0; index < rectangle.positions.size(); ++index) {
        if (!finite(rectangle.positions[index]) || !finite(rectangle.corner[index]))
            return false;
    }
    return std::fabs(area2(rectangle.positions[0], rectangle.positions[1],
                           rectangle.positions[3])) > 0.000001f ||
           std::fabs(area2(rectangle.positions[0], rectangle.positions[3],
                           rectangle.positions[2])) > 0.000001f;
}

bool resolve_transformed_s16_rectangle(const TransformedS16RectangleLayout& layout,
                                       std::array<Vec2, 4>& positions) noexcept {
    if (!valid(layout.transform))
        return false;
    const float x1 = narrow_s16(layout.x1);
    const float y1 = narrow_s16(layout.y1);
    const float x2 = narrow_s16(layout.x2);
    const float y2 = narrow_s16(layout.y2);
    positions = {
        transform_point(layout.transform, x1, y1), transform_point(layout.transform, x2, y1),
        transform_point(layout.transform, x1, y2), transform_point(layout.transform, x2, y2)};
    return std::ranges::all_of(positions, [](Vec2 position) { return finite(position); });
}

bool valid(const SolidRectangleDraw& draw) noexcept {
    return valid(draw.canvas) && valid(draw.rectangle);
}

SolidRectangleMesh make_mesh(const SolidRectangleCommand& rectangle) noexcept {
    constexpr std::array<std::size_t, 6> kCornerOrder{0, 1, 3, 0, 3, 2};
    SolidRectangleMesh mesh{};
    for (std::size_t index = 0; index < mesh.size(); ++index) {
        const std::size_t corner = kCornerOrder[index];
        mesh[index] = {rectangle.positions[corner], {}, rectangle.corner[corner]};
    }
    return mesh;
}

} // namespace sb::native_render
