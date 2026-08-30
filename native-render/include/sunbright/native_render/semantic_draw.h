#pragma once

#include <sunbright/native_render/picture.h>
#include <sunbright/native_render/solid_rectangle.h>

#include <type_traits>
#include <variant>

namespace sb::native_render {

using SemanticDraw = std::variant<PictureDraw, SolidRectangleDraw>;

[[nodiscard]] inline bool valid(const SemanticDraw& draw) noexcept {
    return std::visit([](const auto& value) { return valid(value); }, draw);
}

[[nodiscard]] inline const Canvas& canvas(const SemanticDraw& draw) noexcept {
    return std::visit([](const auto& value) -> const Canvas& { return value.canvas; }, draw);
}

[[nodiscard]] inline const ClipRect& clip(const SemanticDraw& draw) noexcept {
    return std::visit(
        [](const auto& value) -> const ClipRect& {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, PictureDraw>)
                return value.picture.clip;
            else
                return value.rectangle.clip;
        },
        draw);
}

} // namespace sb::native_render
