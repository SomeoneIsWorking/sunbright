#include <sunbright/native_render/semantic_2d_types.h>

#include <algorithm>
#include <cmath>

namespace sb::native_render {
namespace {

bool finite(Vec2 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

} // namespace

Color color_from_rgba8(std::uint32_t rgba) noexcept {
    constexpr float kScale = 1.0f / 255.0f;
    return {static_cast<float>((rgba >> 24U) & 0xffU) * kScale,
            static_cast<float>((rgba >> 16U) & 0xffU) * kScale,
            static_cast<float>((rgba >> 8U) & 0xffU) * kScale,
            static_cast<float>(rgba & 0xffU) * kScale};
}

bool valid(const Canvas& canvas) noexcept {
    return finite(canvas.origin) && finite(canvas.extent) && canvas.extent.x > 0.0f &&
           canvas.extent.y > 0.0f && canvas.viewport.width != 0 && canvas.viewport.height != 0;
}

bool valid(const Matrix3x4& matrix) noexcept {
    return std::ranges::all_of(matrix.value, [](float value) { return std::isfinite(value); });
}

Matrix3x4 concatenate_transform(const Matrix3x4& first, const Matrix3x4& second) noexcept {
    Matrix3x4 result{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            float value = column == 3 ? first.value[row * 4 + 3] : 0.0f;
            for (std::size_t inner = 0; inner < 3; ++inner)
                value += first.value[row * 4 + inner] * second.value[inner * 4 + column];
            result.value[row * 4 + column] = value;
        }
    }
    return result;
}

Vec2 transform_point(const Matrix3x4& matrix, float x, float y) noexcept {
    return {matrix.value[0] * x + matrix.value[1] * y + matrix.value[3],
            matrix.value[4] * x + matrix.value[5] * y + matrix.value[7]};
}

bool resolve_scissor(const Canvas& canvas, const ClipRect& clip, std::uint32_t targetWidth,
                     std::uint32_t targetHeight, PixelRect& scissor) noexcept {
    if (!valid(canvas) || targetWidth == 0 || targetHeight == 0)
        return false;
    if (canvas.viewport.x < 0 || canvas.viewport.y < 0 ||
        static_cast<std::uint64_t>(canvas.viewport.x) + canvas.viewport.width > targetWidth ||
        static_cast<std::uint64_t>(canvas.viewport.y) + canvas.viewport.height > targetHeight) {
        return false;
    }
    if (clip.enabled && (clip.width == 0 || clip.height == 0))
        return false;

    const float scaleX = static_cast<float>(canvas.viewport.width) / canvas.extent.x;
    const float scaleY = static_cast<float>(canvas.viewport.height) / canvas.extent.y;
    const auto clampX = [&](float value) {
        return std::clamp(value, 0.0f, static_cast<float>(targetWidth));
    };
    const auto clampY = [&](float value) {
        return std::clamp(value, 0.0f, static_cast<float>(targetHeight));
    };
    const float logicalLeft = clip.enabled ? static_cast<float>(clip.x) : canvas.origin.x;
    const float logicalTop = clip.enabled ? static_cast<float>(clip.y) : canvas.origin.y;
    const float logicalRight = clip.enabled
                                   ? static_cast<float>(clip.x) + static_cast<float>(clip.width)
                                   : canvas.origin.x + canvas.extent.x;
    const float logicalBottom = clip.enabled
                                    ? static_cast<float>(clip.y) + static_cast<float>(clip.height)
                                    : canvas.origin.y + canvas.extent.y;
    const float viewportX = static_cast<float>(canvas.viewport.x);
    const float viewportY = static_cast<float>(canvas.viewport.y);
    const float left = clampX(viewportX + (logicalLeft - canvas.origin.x) * scaleX);
    const float top = clampY(viewportY + (logicalTop - canvas.origin.y) * scaleY);
    const float right = clampX(viewportX + (logicalRight - canvas.origin.x) * scaleX);
    const float bottom = clampY(viewportY + (logicalBottom - canvas.origin.y) * scaleY);
    const auto x = static_cast<std::int32_t>(std::floor(left));
    const auto y = static_cast<std::int32_t>(std::floor(top));
    const auto rightPixel = static_cast<std::int32_t>(std::ceil(right));
    const auto bottomPixel = static_cast<std::int32_t>(std::ceil(bottom));
    if (rightPixel <= x || bottomPixel <= y)
        return false;
    scissor = {x, y, static_cast<std::uint32_t>(rightPixel - x),
               static_cast<std::uint32_t>(bottomPixel - y)};
    return true;
}

} // namespace sb::native_render
