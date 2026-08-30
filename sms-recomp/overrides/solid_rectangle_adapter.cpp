#include "solid_rectangle_adapter.h"

#include <array>
#include <bit>
#include <cstddef>

namespace sb::recomp {
namespace {

bool read_s32(const GuestByteReader& reader, std::uint32_t address, std::int32_t& value) noexcept {
    std::array<std::uint8_t, 4> bytes{};
    if (reader.read == nullptr || !reader.read(reader.context, address, bytes.data(), bytes.size()))
        return false;
    const std::uint32_t raw = (static_cast<std::uint32_t>(bytes[0]) << 24U) |
                              (static_cast<std::uint32_t>(bytes[1]) << 16U) |
                              (static_cast<std::uint32_t>(bytes[2]) << 8U) | bytes[3];
    value = std::bit_cast<std::int32_t>(raw);
    return true;
}

} // namespace

bool capture_fill_rectangle(const GuestByteReader& reader, std::uint32_t rect, std::uint32_t rgba,
                            native_render::SolidRectangleDraw& draw) noexcept {
    std::int32_t x1 = 0;
    std::int32_t y1 = 0;
    std::int32_t x2 = 0;
    std::int32_t y2 = 0;
    if (rect == 0 || !read_s32(reader, rect, x1) || !read_s32(reader, rect + 4U, y1) ||
        !read_s32(reader, rect + 8U, x2) || !read_s32(reader, rect + 12U, y2)) {
        return false;
    }
    const native_render::Color color = native_render::color_from_rgba8(rgba);
    draw = {{{0.0f, 0.0f}, {640.0f, 480.0f}, {0, 0, 640, 480}},
            {.instance = rect,
             .source = native_render::SolidRectangleSource::Gc2dFillRect,
             .positions = {native_render::Vec2{static_cast<float>(x1), static_cast<float>(y1)},
                           native_render::Vec2{static_cast<float>(x2), static_cast<float>(y1)},
                           native_render::Vec2{static_cast<float>(x1), static_cast<float>(y2)},
                           native_render::Vec2{static_cast<float>(x2), static_cast<float>(y2)}},
             .corner = {color, color, color, color}}};
    return native_render::valid(draw);
}

} // namespace sb::recomp
