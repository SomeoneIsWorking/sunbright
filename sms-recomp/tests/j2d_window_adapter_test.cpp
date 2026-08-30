#include "../overrides/j2d_window_adapter.h"

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint32_t kBase = 0x80000000;

class GuestMemory {
  public:
    explicit GuestMemory(std::size_t size) : bytes_(size) {}
    sb::recomp::GuestByteReader reader() noexcept { return {.context = this, .read = read}; }
    void u8(std::uint32_t address, std::uint8_t value) { bytes_[offset(address)] = value; }
    void u16(std::uint32_t address, std::uint16_t value) {
        u8(address, static_cast<std::uint8_t>(value >> 8U));
        u8(address + 1, static_cast<std::uint8_t>(value));
    }
    void u32(std::uint32_t address, std::uint32_t value) {
        u8(address, static_cast<std::uint8_t>(value >> 24U));
        u8(address + 1, static_cast<std::uint8_t>(value >> 16U));
        u8(address + 2, static_cast<std::uint8_t>(value >> 8U));
        u8(address + 3, static_cast<std::uint8_t>(value));
    }
    void s32(std::uint32_t address, std::int32_t value) {
        u32(address, std::bit_cast<std::uint32_t>(value));
    }
    void f32(std::uint32_t address, float value) {
        u32(address, std::bit_cast<std::uint32_t>(value));
    }
    void matrix(std::uint32_t address, const std::array<float, 12>& values) {
        for (std::size_t index = 0; index < values.size(); ++index)
            f32(address + static_cast<std::uint32_t>(index * 4), values[index]);
    }

  private:
    static bool read(void* context, std::uint32_t address, void* destination,
                     std::size_t size) noexcept {
        auto& memory = *static_cast<GuestMemory*>(context);
        if (address < kBase ||
            static_cast<std::uint64_t>(address - kBase) + size > memory.bytes_.size())
            return false;
        std::memcpy(destination, memory.bytes_.data() + (address - kBase), size);
        return true;
    }
    std::size_t offset(std::uint32_t address) const {
        assert(address >= kBase && address - kBase < bytes_.size());
        return address - kBase;
    }
    std::vector<std::uint8_t> bytes_;
};

void rect(GuestMemory& memory, std::uint32_t address, std::int32_t x1, std::int32_t y1,
          std::int32_t x2, std::int32_t y2) {
    memory.s32(address, x1);
    memory.s32(address + 4, y1);
    memory.s32(address + 8, x2);
    memory.s32(address + 12, y2);
}

void texture(GuestMemory& memory, std::uint32_t address, std::uint32_t resource,
             std::uint32_t texels) {
    memory.u32(address + 0x20, resource);
    memory.u32(address + 0x24, texels);
    memory.u32(address + 0x2c, 0);
    memory.u32(address + 0x34, 6); // RGBA8
    memory.u32(address + 0x38, 1);
    memory.u16(address + 0x3c, 4);
    memory.u16(address + 0x3e, 4);
    memory.u8(address + 0x40, 0);
    memory.u8(address + 0x41, 0);
    memory.u8(address + 0x42, 1);
    memory.u8(address + 0x43, 1);
    memory.u8(texels, 0xff);
}

} // namespace

int main() {
    constexpr std::uint32_t self = kBase + 0x1000;
    constexpr std::uint32_t outer = kBase + 0x2000;
    constexpr std::uint32_t contents = kBase + 0x2100;
    constexpr std::uint32_t parent = kBase + 0x2200;
    constexpr std::array<std::uint32_t, 5> textures{kBase + 0x3000, kBase + 0x3100, kBase + 0x3200,
                                                    kBase + 0x3300, kBase + 0x3400};
    GuestMemory memory(0x10000);

    rect(memory, outer, 30, 40, 50, 56);
    rect(memory, contents, 4, 3, 16, 13);
    memory.matrix(parent, {1, 0, 0, 2, 0, 1, 0, 3, 0, 0, 1, 0});
    memory.matrix(self + 0x84, {1, 0, 0, 5, 0, 1, 0, 7, 0, 0, 1, 0});
    memory.u8(self + 0xcd, 128);
    memory.u32(self + 0x110, textures[0]);
    for (std::size_t index = 0; index < 4; ++index)
        memory.u32(self + 0x100 + static_cast<std::uint32_t>(index * 4), textures[index + 1]);
    memory.u32(self + 0x114, 0);
    memory.u32(self + 0x118, 0xff0000ff);
    memory.u32(self + 0x11c, 0x00ff0080);
    memory.u32(self + 0x120, 0x0000ffff);
    memory.u32(self + 0x124, 0xffffffff);
    memory.u32(self + 0x128, 0xf0e0d0c0);
    memory.u32(self + 0x12c, 0x10203040);
    memory.s32(self + 0x130, 8);
    memory.s32(self + 0x134, 8);
    for (std::size_t index = 0; index < textures.size(); ++index)
        texture(memory, textures[index], kBase + 0x5000 + static_cast<std::uint32_t>(index * 0x100),
                kBase + 0x7000 + static_cast<std::uint32_t>(index * 0x100));

    sb::recomp::CapturedWindow capture{};
    assert(sb::recomp::capture_j2d_window(memory.reader(), self, outer, contents, parent,
                                          capture) == sb::recomp::WindowCaptureResult::Visible);
    assert(capture.hasContents);
    assert(capture.contents.source == sb::native_render::SolidRectangleSource::J2dWindowContents);
    assert((capture.contents.positions[0] == sb::native_render::Vec2{11, 13}));
    assert((capture.contents.positions[3] == sb::native_render::Vec2{23, 23}));
    assert(capture.contents.corner[0].a == 128.0f / 255.0f);
    assert(capture.contents.corner[1].a == 64.0f / 255.0f);
    assert(capture.pictureCount == 9);
    assert(capture.pictures[0].textureIndex == 0);
    assert(capture.pictures[0].command.source == sb::native_render::PictureSource::J2dWindow);
    assert(capture.pictures[1].textureIndex == 1);
    assert(capture.pictures[5].textureIndex == 2);
    assert((capture.pictures[5].command.uv[0] == sb::native_render::Vec2{0, 0}));
    assert((capture.pictures[5].command.uv[3] == sb::native_render::Vec2{0, 1}));
    assert(capture.pictures[1].command.material.black ==
           sb::native_render::color_from_rgba8(0x10203040));
    assert(capture.image_for(capture.pictures[0]).rgba8.size() == 4U * 4U * 4U);

    // Known-negative control: the retail size gate must produce a deliberate no-op, not a broken
    // command that happens to look empty.
    rect(memory, outer, 30, 40, 37, 56);
    assert(sb::recomp::capture_j2d_window(memory.reader(), self, outer, contents, parent,
                                          capture) == sb::recomp::WindowCaptureResult::Culled);
    rect(memory, outer, 30, 40, 50, 56);

    // Parser falsifier: unsupported sampler state must make the adapter say Invalid.
    memory.u8(textures[2] + 0x42, 9);
    assert(sb::recomp::capture_j2d_window(memory.reader(), self, outer, contents, parent,
                                          capture) == sb::recomp::WindowCaptureResult::Invalid);
    return 0;
}
