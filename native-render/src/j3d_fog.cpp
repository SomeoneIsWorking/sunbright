#include <sunbright/native_render/j3d_fog.h>

#include <cmath>

namespace sb::native_render {
namespace {

bool supported_type(std::uint8_t type) noexcept {
    switch (type) {
    case 2:
    case 4:
    case 5:
    case 6:
    case 7:
    case 10:
    case 12:
    case 13:
    case 14:
    case 15:
        return true;
    default:
        return false;
    }
}

} // namespace

J3dFogResult classify_j3d_fog(const J3dFogState& fog) noexcept {
    if (fog.type == 0)
        return J3dFogResult::Disabled;
    if (!supported_type(fog.type) || !std::isfinite(fog.start) || !std::isfinite(fog.end) ||
        !std::isfinite(fog.near) || !std::isfinite(fog.far) || fog.near >= fog.far ||
        fog.start >= fog.end) {
        return J3dFogResult::Invalid;
    }
    if (fog.type == 2 && !fog.rangeAdjustmentEnabled)
        return J3dFogResult::Linear;
    return J3dFogResult::Unsupported;
}

bool build_model_fog(const J3dFogState& fog, ModelFog& modelFog) noexcept {
    const J3dFogResult result = classify_j3d_fog(fog);
    if (result == J3dFogResult::Disabled) {
        modelFog = {};
        return true;
    }
    if (result != J3dFogResult::Linear)
        return false;
    modelFog = {
        .mode = ModelFogMode::Linear,
        .start = fog.start,
        .end = fog.end,
        .color = color_from_rgba8(fog.colorRgba8),
    };
    return true;
}

} // namespace sb::native_render
