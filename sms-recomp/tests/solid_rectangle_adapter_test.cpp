#include "../overrides/solid_rectangle_adapter.h"

#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

constexpr std::uint32_t kBase = 0x80001000;

class GuestRect {
  public:
    sb::recomp::GuestByteReader reader() noexcept { return {.context = this, .read = read}; }

    void s32(std::size_t offset, std::int32_t value) {
        const std::uint32_t raw = std::bit_cast<std::uint32_t>(value);
        bytes_[offset] = static_cast<std::uint8_t>(raw >> 24U);
        bytes_[offset + 1] = static_cast<std::uint8_t>(raw >> 16U);
        bytes_[offset + 2] = static_cast<std::uint8_t>(raw >> 8U);
        bytes_[offset + 3] = static_cast<std::uint8_t>(raw);
    }

  private:
    static bool read(void* context, std::uint32_t address, void* destination,
                     std::size_t size) noexcept {
        auto& memory = *static_cast<GuestRect*>(context);
        if (address < kBase || static_cast<std::uint64_t>(address - kBase) + size > 16U)
            return false;
        std::memcpy(destination, memory.bytes_.data() + address - kBase, size);
        return true;
    }

    std::array<std::uint8_t, 16> bytes_{};
};

} // namespace

int main() {
    GuestRect memory;
    memory.s32(0, -107);
    memory.s32(4, 20);
    memory.s32(8, 747);
    memory.s32(12, 460);

    sb::native_render::SolidRectangleDraw draw{};
    assert(sb::recomp::capture_fill_rectangle(memory.reader(), kBase, 0x10203080U, draw));
    assert(draw.canvas.extent == sb::native_render::Vec2(640.0f, 480.0f));
    assert(draw.rectangle.positions[0] == sb::native_render::Vec2(-107.0f, 20.0f));
    assert(draw.rectangle.positions[1] == sb::native_render::Vec2(747.0f, 20.0f));
    assert(draw.rectangle.positions[2] == sb::native_render::Vec2(-107.0f, 460.0f));
    assert(draw.rectangle.positions[3] == sb::native_render::Vec2(747.0f, 460.0f));
    const auto color = draw.rectangle.corner[0];
    constexpr float kColorTolerance = 0.000001f;
    assert(std::abs(color.r - 16.0f / 255.0f) < kColorTolerance);
    assert(std::abs(color.g - 32.0f / 255.0f) < kColorTolerance);
    assert(std::abs(color.b - 48.0f / 255.0f) < kColorTolerance);
    assert(std::abs(color.a - 128.0f / 255.0f) < kColorTolerance);

    assert(!sb::recomp::capture_fill_rectangle(memory.reader(), kBase + 4U, 0xffffffffU, draw));
    memory.s32(8, -107);
    assert(!sb::recomp::capture_fill_rectangle(memory.reader(), kBase, 0xffffffffU, draw));
}
