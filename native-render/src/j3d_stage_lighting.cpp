#include <sunbright/native_render/j3d_stage_lighting.h>

#include <cmath>

namespace sb::native_render {
namespace {

thread_local ModelLightingContext g_currentLighting{};
thread_local bool g_hasCurrentLighting = false;

Vec3 transform_position(const Matrix3x4& matrix, Vec3 position) noexcept {
    return {
        matrix.value[0] * position.x + matrix.value[1] * position.y + matrix.value[2] * position.z +
            matrix.value[3],
        matrix.value[4] * position.x + matrix.value[5] * position.y + matrix.value[6] * position.z +
            matrix.value[7],
        matrix.value[8] * position.x + matrix.value[9] * position.y +
            matrix.value[10] * position.z + matrix.value[11],
    };
}

Vec3 normalized(Vec3 value) noexcept {
    const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (length <= 0.0F)
        return {0.0F, 0.0F, 1.0F};
    return {value.x / length, value.y / length, value.z / length};
}

} // namespace

ModelLightingContext build_j3d_stage_lighting(const J3dStageLightingInput& input) noexcept {
    ModelLightingContext lighting{};
    lighting.ambientColor = input.ambientColor;
    lighting.pointLights[0] = {
        .position = transform_position(input.view, input.primaryWorldPosition),
        .color = input.primaryColor,
        .distanceAttenuation = {1.0F, 0.0F, 0.0F},
    };
    lighting.specular = {
        .directionToLight = normalized(lighting.pointLights[0].position),
        .color = input.primaryColor,
        .shininess = input.shininess,
    };
    lighting.pointLightCount = 1;
    if (input.effectEnabled) {
        // The game's effect light is authored as medium falloff with half intensity at 1000 world
        // units. These are the ordinary quadratic coefficients for that authored curve.
        lighting.pointLights[1] = {
            .position = transform_position(input.view, input.effectWorldPosition),
            .color = input.effectColor,
            .distanceAttenuation = {1.0F, 0.0005F, 0.0000005F},
        };
        lighting.pointLightCount = 2;
    }
    return lighting;
}

void publish_j3d_stage_lighting(const J3dStageLightingInput& input) noexcept {
    g_currentLighting = build_j3d_stage_lighting(input);
    g_hasCurrentLighting = true;
}

void clear_j3d_stage_lighting() noexcept {
    g_currentLighting = {};
    g_hasCurrentLighting = false;
}

const ModelLightingContext* current_j3d_stage_lighting() noexcept {
    return g_hasCurrentLighting ? &g_currentLighting : nullptr;
}

} // namespace sb::native_render
