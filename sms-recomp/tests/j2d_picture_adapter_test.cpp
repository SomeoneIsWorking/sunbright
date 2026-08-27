#include "../overrides/j2d_picture_adapter.h"

#include <array>
#include <bit>
#include <cassert>
#include <cmath>
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

bool close(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001f;
}

void write_texture(GuestMemory& memory, std::uint32_t texture, std::uint32_t resource,
                   std::uint16_t width, std::uint16_t height, bool alpha) {
    memory.u32(texture + 0x20, resource);
    memory.u32(texture + 0x38, alpha ? 1 : 0);
    memory.u16(texture + 0x3c, width);
    memory.u16(texture + 0x3e, height);
    memory.u8(texture + 0x40, 1); // repeat
    memory.u8(texture + 0x41, 2); // mirror
    memory.u8(texture + 0x42, 5); // linear + linear mip
    memory.u8(texture + 0x43, 1); // linear
}

} // namespace

int main() {
    constexpr std::uint32_t self = kBase + 0x1000;
    constexpr std::uint32_t parentMatrix = kBase + 0x2000;
    constexpr std::uint32_t texture0 = kBase + 0x3000;
    constexpr std::uint32_t texture1 = kBase + 0x3100;
    GuestMemory memory(0x5000);

    memory.s32(self + 0x14, 40);
    memory.s32(self + 0x18, 70);
    memory.s32(self + 0x1c, 140);
    memory.s32(self + 0x20, 120);
    memory.u8(self + 0xcd, 128);
    memory.u32(self + 0xec, texture0);
    memory.u32(self + 0xf0, texture1);
    memory.u8(self + 0xfc, 2);
    memory.u32(self + 0x128, 0);
    memory.u32(self + 0x12c, 0);
    memory.u8(self + 0x130, 0);
    memory.s32(self + 0x134, 0);
    memory.s32(self + 0x138, 0);
    memory.u32(self + 0x13c, 0xf0e0d0c0);
    memory.u32(self + 0x140, 0x10203040);
    memory.u32(self + 0x144, 0xff0000ff);
    memory.u32(self + 0x148, 0x00ff00ff);
    memory.u32(self + 0x14c, 0x0000ffff);
    memory.u32(self + 0x150, 0xffffffff);
    memory.u32(self + 0x154, 0x00000080);
    memory.u32(self + 0x158, 0x00000040);
    memory.matrix(self + 0x84, {1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 0});
    memory.matrix(parentMatrix, {1, 0, 0, 3, 0, 1, 0, 4, 0, 0, 1, 0});
    write_texture(memory, texture0, kBase + 0x4000, 64, 32, true);
    write_texture(memory, texture1, kBase + 0x4200, 32, 16, false);

    sb::native_render::PictureCommand command{};
    assert(sb::recomp::capture_j2d_picture(memory.reader(), self, parentMatrix, command));
    assert(command.instance == self);
    assert(command.material.textureCount == 2);
    assert(command.material.textures[0].resource == kBase + 0x4000);
    assert(command.material.textures[0].addressU == sb::native_render::AddressMode::Repeat);
    assert(command.material.textures[0].addressV == sb::native_render::AddressMode::Mirror);
    assert(command.material.textures[0].minFilter == sb::native_render::FilterMode::Linear);
    assert(command.material.textures[0].mipFilter == sb::native_render::MipFilter::Linear);
    assert(command.material.textures[0].hasAlpha);
    assert(!command.material.textures[1].hasAlpha);
    assert(close(command.material.textures[1].colorMix, 128.0f / 255.0f));
    assert(close(command.material.textures[1].alphaMix, 64.0f / 255.0f));
    assert(close(command.opacity, 128.0f / 255.0f));
    assert(!command.clip.enabled);
    assert((command.positions ==
            std::array<sb::native_render::Vec2, 4>{
                sb::native_render::Vec2{31, 33}, sb::native_render::Vec2{95, 33},
                sb::native_render::Vec2{31, 65}, sb::native_render::Vec2{95, 65}}));
    assert((command.uv == std::array<sb::native_render::Vec2, 4>{
                              sb::native_render::Vec2{0, 0}, sb::native_render::Vec2{1, 0},
                              sb::native_render::Vec2{0, 1}, sb::native_render::Vec2{1, 1}}));

    // Known-negative control: an invalid GameCube sampler value must refuse the entire command,
    // proving this parser does not silently turn unread/unsupported state into a plausible draw.
    memory.u8(texture0 + 0x42, 9);
    assert(!sb::recomp::capture_j2d_picture(memory.reader(), self, parentMatrix, command));

    // An unmapped matrix must fail too; the retained game body can still run, but no semantic draw
    // is published from incomplete data.
    assert(!sb::recomp::capture_j2d_picture(memory.reader(), self, kBase + 0x6000, command));
}
