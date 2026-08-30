#include "j2d_fill_box_adapter.h"

namespace sb::recomp {

bool capture_j2d_fill_box(const GuestByteReader& byteReader, std::uint32_t self, std::uint32_t rect,
                          const native_render::PictureContext& context,
                          native_render::SolidRectangleDraw& draw) noexcept {
    if (self == 0 || rect == 0 || !native_render::valid(context.canvas))
        return false;

    const BigEndianGuestReader reader(byteReader);
    native_render::TransformedS16RectangleLayout layout{};
    if (!reader.s32(rect, layout.x1) || !reader.s32(rect + 4U, layout.y1) ||
        !reader.s32(rect + 8U, layout.x2) || !reader.s32(rect + 12U, layout.y2)) {
        return false;
    }
    for (std::size_t index = 0; index < layout.transform.value.size(); ++index) {
        if (!reader.f32(self + 0x84U + static_cast<std::uint32_t>(index * sizeof(float)),
                        layout.transform.value[index])) {
            return false;
        }
    }

    native_render::SolidRectangleDraw result{};
    result.canvas = context.canvas;
    result.rectangle.instance = self;
    result.rectangle.source = native_render::SolidRectangleSource::J2dGrafContextFillBox;
    result.rectangle.clip = context.scissor;
    if (!native_render::resolve_transformed_s16_rectangle(layout, result.rectangle.positions))
        return false;

    // Semantic corner order is TL, TR, BL, BR. Retail's third and fourth FIFO colours are BL and
    // BR respectively, despite their member names describing the opposite geometric corners.
    constexpr std::array<std::uint32_t, 4> kColorOffsets{0x28U, 0x2cU, 0x30U, 0x34U};
    for (std::size_t index = 0; index < kColorOffsets.size(); ++index) {
        std::uint32_t rgba = 0;
        if (!reader.u32(self + kColorOffsets[index], rgba))
            return false;
        result.rectangle.corner[index] = native_render::color_from_rgba8(rgba);
    }
    if (!native_render::valid(result))
        return false;
    draw = result;
    return true;
}

} // namespace sb::recomp
