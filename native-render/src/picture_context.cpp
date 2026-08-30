#include <sunbright/native_render/picture_context.h>

#include <algorithm>
#include <cstdint>

namespace sb::native_render {

ClipRect j2d_target_scissor(std::int32_t x1, std::int32_t y1, std::int32_t x2,
                            std::int32_t y2) noexcept {
    std::int64_t left = std::min(x1, x2);
    std::int64_t right = std::max(x1, x2);
    std::int64_t top = static_cast<std::int64_t>(std::min(y1, y2)) - 1;
    std::int64_t bottom = static_cast<std::int64_t>(std::max(y1, y2)) - 1;
    left = std::clamp<std::int64_t>(left, 0, 1024);
    right = std::clamp<std::int64_t>(right, 0, 1024);
    top = std::clamp<std::int64_t>(top, 0, 1000);
    bottom = std::clamp<std::int64_t>(bottom, 0, 1000);
    const std::uint32_t width = right > left ? static_cast<std::uint32_t>(right - left) : 0;
    const std::uint32_t height = bottom > top ? static_cast<std::uint32_t>(bottom - top) : 0;
    return {.enabled = true,
            .x = static_cast<std::int32_t>(left),
            .y = static_cast<std::int32_t>(top),
            .width = width,
            .height = height,
            .space = ClipCoordinateSpace::TargetPixels};
}

bool PictureContextStack::push(PictureContext context) noexcept {
    if (!valid(context.canvas) || depth_ == contexts_.size())
        return false;
    contexts_[depth_++] = context;
    return true;
}

bool PictureContextStack::pop() noexcept {
    if (depth_ == 0)
        return false;
    --depth_;
    return true;
}

const PictureContext* PictureContextStack::current() const noexcept {
    return depth_ == 0 ? nullptr : &contexts_[depth_ - 1];
}

std::size_t PictureContextStack::depth() const noexcept {
    return depth_;
}

} // namespace sb::native_render
