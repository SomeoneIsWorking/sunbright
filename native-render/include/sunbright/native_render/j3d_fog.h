#pragma once

#include <sunbright/native_render/model.h>

#include <array>
#include <cstdint>

namespace sb::native_render {

// High-level J3D fog values copied by either runtime adapter. This is the authored fog contract,
// not the packed GameCube coefficient/register representation used by the compatibility renderer.
struct J3dFogState {
    std::uint8_t type = 0;
    bool rangeAdjustmentEnabled = false;
    std::uint16_t center = 0;
    float start = 0.0F;
    float end = 0.0F;
    float near = 0.0F;
    float far = 0.0F;
    std::uint32_t colorRgba8 = 0;
    std::array<std::uint16_t, 10> rangeAdjustmentTable{};
    bool operator==(const J3dFogState&) const = default;
};

enum class J3dFogResult : std::uint8_t { Disabled, Linear, Unsupported, Invalid };

[[nodiscard]] J3dFogResult classify_j3d_fog(const J3dFogState& fog) noexcept;
[[nodiscard]] bool build_model_fog(const J3dFogState& fog, ModelFog& modelFog) noexcept;

} // namespace sb::native_render
