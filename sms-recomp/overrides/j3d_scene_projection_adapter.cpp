#include "j3d_scene_projection_adapter.h"

#include <cstddef>

namespace sb::recomp {
namespace {

constexpr std::uint32_t kProjectionOffset = 0x74;

} // namespace

GuestJ3dSceneProjectionResult
capture_guest_j3d_scene_projection(const GuestByteReader& reader, std::uint32_t graphics,
                                   native_render::ModelSceneContext& context) noexcept {
    native_render::Matrix4x4 projection{};
    const BigEndianGuestReader memory(reader);
    for (std::size_t index = 0; index < projection.value.size(); ++index) {
        if (!memory.f32(graphics + kProjectionOffset + static_cast<std::uint32_t>(index * 4),
                        projection.value[index])) {
            return GuestJ3dSceneProjectionResult::Unreadable;
        }
    }

    switch (native_render::capture_j3d_scene_context(projection, context)) {
    case native_render::J3dProjectionResult::Perspective:
        return GuestJ3dSceneProjectionResult::Perspective;
    case native_render::J3dProjectionResult::Orthographic:
        return GuestJ3dSceneProjectionResult::Orthographic;
    case native_render::J3dProjectionResult::NonFinite:
        return GuestJ3dSceneProjectionResult::NonFinite;
    case native_render::J3dProjectionResult::Unsupported:
        return GuestJ3dSceneProjectionResult::Unsupported;
    }
    return GuestJ3dSceneProjectionResult::Unsupported;
}

} // namespace sb::recomp
