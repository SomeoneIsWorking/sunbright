#include "../overrides/j3d_scene_projection_adapter.h"

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

constexpr std::uint32_t kGraphics = 0x80001000;
constexpr std::size_t kMemorySize = 512;

class GuestMemory {
  public:
    void f32(std::uint32_t address, float value) {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
        const std::size_t offset = address - kGraphics;
        bytes_[offset] = static_cast<std::uint8_t>(bits >> 24U);
        bytes_[offset + 1] = static_cast<std::uint8_t>(bits >> 16U);
        bytes_[offset + 2] = static_cast<std::uint8_t>(bits >> 8U);
        bytes_[offset + 3] = static_cast<std::uint8_t>(bits);
    }

    void projection(const std::array<float, 16>& values) {
        for (std::size_t index = 0; index < values.size(); ++index)
            f32(kGraphics + 0x74 + static_cast<std::uint32_t>(index * 4), values[index]);
    }

    sb::recomp::GuestByteReader reader() noexcept { return {.context = this, .read = read}; }

    std::size_t readableSize = kMemorySize;

  private:
    static bool read(void* context, std::uint32_t address, void* destination,
                     std::size_t size) noexcept {
        auto& memory = *static_cast<GuestMemory*>(context);
        if (address < kGraphics || address - kGraphics + size > memory.readableSize)
            return false;
        std::memcpy(destination, memory.bytes_.data() + (address - kGraphics), size);
        return true;
    }

    std::array<std::uint8_t, kMemorySize> bytes_{};
};

} // namespace

int main() {
    using namespace sb;

    GuestMemory memory{};
    memory.projection({2, 0, 0, 0, 0, 3, 0, 0, 0, 0, -0.25F, -2, 0, 0, -1, 0});
    native_render::ModelSceneContext context{};
    assert(recomp::capture_guest_j3d_scene_projection(memory.reader(), kGraphics, context) ==
           recomp::GuestJ3dSceneProjectionResult::Perspective);
    assert(context.projectionKind == native_render::ProjectionKind::Perspective);
    assert(context.projection.value[10] == -1.25F);

    memory.projection({0.5F, 0, 0, -1, 0, 0.5F, 0, 1, 0, 0, -0.25F, -1, 0, 0, 0, 1});
    assert(recomp::capture_guest_j3d_scene_projection(memory.reader(), kGraphics, context) ==
           recomp::GuestJ3dSceneProjectionResult::Orthographic);
    assert(context.projectionKind == native_render::ProjectionKind::Orthographic);

    memory.f32(kGraphics + 0x74 + 14 * 4, -0.5F);
    assert(recomp::capture_guest_j3d_scene_projection(memory.reader(), kGraphics, context) ==
           recomp::GuestJ3dSceneProjectionResult::Unsupported);

    memory.readableSize = 0x74 + 15 * 4;
    assert(recomp::capture_guest_j3d_scene_projection(memory.reader(), kGraphics, context) ==
           recomp::GuestJ3dSceneProjectionResult::Unreadable);
}
