#include "../overrides/j2d_fill_box_adapter.h"

#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

constexpr std::uint32_t kBase = 0x80002000;
constexpr std::uint32_t kContext = kBase;
constexpr std::uint32_t kRect = kBase + 0x100;

class GuestMemory {
  public:
    sb::recomp::GuestByteReader reader() noexcept { return {.context = this, .read = read}; }

    void u32(std::size_t offset, std::uint32_t value) {
        bytes_[offset] = static_cast<std::uint8_t>(value >> 24U);
        bytes_[offset + 1] = static_cast<std::uint8_t>(value >> 16U);
        bytes_[offset + 2] = static_cast<std::uint8_t>(value >> 8U);
        bytes_[offset + 3] = static_cast<std::uint8_t>(value);
    }

    void s32(std::size_t offset, std::int32_t value) {
        u32(offset, std::bit_cast<std::uint32_t>(value));
    }

    void f32(std::size_t offset, float value) { u32(offset, std::bit_cast<std::uint32_t>(value)); }

  private:
    static bool read(void* context, std::uint32_t address, void* destination,
                     std::size_t size) noexcept {
        auto& memory = *static_cast<GuestMemory*>(context);
        if (address < kBase ||
            static_cast<std::uint64_t>(address - kBase) + size > memory.bytes_.size()) {
            return false;
        }
        std::memcpy(destination, memory.bytes_.data() + address - kBase, size);
        return true;
    }

    std::array<std::uint8_t, 0x140> bytes_{};
};

bool near(float actual, float expected) {
    return std::abs(actual - expected) < 0.000001f;
}

} // namespace

int main() {
    GuestMemory memory;
    memory.s32(0x100, 65537);
    memory.s32(0x104, -65534);
    memory.s32(0x108, 65546);
    memory.s32(0x10c, 65556);
    memory.u32(0x28, 0xff0000ff);
    memory.u32(0x2c, 0x00ff00ff);
    memory.u32(0x30, 0x0000ffff);
    memory.u32(0x34, 0xffffffff);
    constexpr std::array<float, 12> transform{2, 0, 0, 5, 0, 3, 0, -4, 0, 0, 1, 0};
    for (std::size_t index = 0; index < transform.size(); ++index)
        memory.f32(0x84 + index * sizeof(float), transform[index]);

    const sb::native_render::PictureContext context{
        {.origin = {0, 0}, .extent = {320, 240}, .viewport = {10, 20, 640, 480}},
        {.enabled = true,
         .x = 12,
         .y = 19,
         .width = 100,
         .height = 50,
         .space = sb::native_render::ClipCoordinateSpace::TargetPixels},
        false};
    sb::native_render::SolidRectangleDraw draw{};
    assert(sb::recomp::capture_j2d_fill_box(memory.reader(), kContext, kRect, context, draw));
    assert(draw.canvas == context.canvas);
    assert(draw.rectangle.source == sb::native_render::SolidRectangleSource::J2dGrafContextFillBox);
    assert(draw.rectangle.clip.space == sb::native_render::ClipCoordinateSpace::TargetPixels);
    assert(draw.rectangle.positions[0] == sb::native_render::Vec2(7, 2));
    assert(draw.rectangle.positions[1] == sb::native_render::Vec2(25, 2));
    assert(draw.rectangle.positions[2] == sb::native_render::Vec2(7, 56));
    assert(draw.rectangle.positions[3] == sb::native_render::Vec2(25, 56));
    assert(near(draw.rectangle.corner[0].r, 1.0f));
    assert(near(draw.rectangle.corner[1].g, 1.0f));
    // Retail emits the mColorBR member at bottom-left and mColorBL at bottom-right.
    assert(near(draw.rectangle.corner[2].b, 1.0f));
    assert(near(draw.rectangle.corner[3].r, 1.0f) && near(draw.rectangle.corner[3].g, 1.0f));

    assert(!sb::recomp::capture_j2d_fill_box(memory.reader(), 0, kRect, context, draw));
    assert(
        !sb::recomp::capture_j2d_fill_box(memory.reader(), kContext, kBase + 0x138, context, draw));
    memory.s32(0x108, 65537);
    assert(!sb::recomp::capture_j2d_fill_box(memory.reader(), kContext, kRect, context, draw));
}
