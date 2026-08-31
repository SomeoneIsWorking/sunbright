#include <native_j3d_lighting_bridge.h>

#include <sunbright/native_render/j3d_stage_lighting.h>
#include <sunbright/native_render/semantic_sink.h>

#include <dolphin/os.h>

#include <algorithm>
#include <cstdint>

namespace {

sb::native_render::Color color_from_bytes(const std::uint8_t* rgba) noexcept {
    constexpr float kByteToUnit = 1.0F / 255.0F;
    return {rgba[0] * kByteToUnit, rgba[1] * kByteToUnit, rgba[2] * kByteToUnit,
            rgba[3] * kByteToUnit};
}

} // namespace

extern "C" void sb_native_j3d_publish_stage_lighting(const SbNativeJ3dStageLighting* lighting) {
    if (!sb::native_render::has_semantic_sink()) {
        sb::native_render::clear_j3d_stage_lighting();
        return;
    }
    if (lighting == nullptr) {
        OSPanic(__FILE__, __LINE__, "native J3D lighting bridge received incomplete stage light");
        return;
    }

    sb::native_render::J3dStageLightingInput input{};
    std::copy_n(lighting->view, input.view.value.size(), input.view.value.begin());
    input.primaryWorldPosition = {lighting->primaryWorldPosition[0],
                                  lighting->primaryWorldPosition[1],
                                  lighting->primaryWorldPosition[2]};
    input.primaryColor = color_from_bytes(lighting->primaryRgba);
    input.ambientColor = color_from_bytes(lighting->ambientRgba);
    input.effectEnabled = lighting->effectEnabled != 0;
    if (input.effectEnabled) {
        input.effectWorldPosition = {lighting->effectWorldPosition[0],
                                     lighting->effectWorldPosition[1],
                                     lighting->effectWorldPosition[2]};
        input.effectColor = color_from_bytes(lighting->effectRgba);
    }
    sb::native_render::publish_j3d_stage_lighting(input);
}
