#include <sunbright/native_render/j3d_fog.h>

#include <cassert>
#include <limits>

int main() {
    using namespace sb::native_render;

    J3dFogState fog{};
    assert(classify_j3d_fog(fog) == J3dFogResult::Disabled);
    ModelFog modelFog{.mode = ModelFogMode::Linear};
    assert(build_model_fog(fog, modelFog));
    assert(modelFog == ModelFog{});

    fog = {.type = 2,
           .start = 199999.0F,
           .end = 200000.0F,
           .near = 10.0F,
           .far = 300000.0F,
           .colorRgba8 = 0xFF0080FF};
    assert(classify_j3d_fog(fog) == J3dFogResult::Linear);
    assert(build_model_fog(fog, modelFog));
    assert(modelFog.mode == ModelFogMode::Linear);
    assert(modelFog.start == 199999.0F);
    assert(modelFog.end == 200000.0F);
    assert(modelFog.color == color_from_rgba8(0xFF0080FF));

    fog.type = 2;
    fog.start = 300.0F;
    fog.end = 1500.0F;
    assert(classify_j3d_fog(fog) == J3dFogResult::Linear);

    fog.start = 299999.0F;
    fog.end = 300000.0F;
    fog.type = 6;
    assert(classify_j3d_fog(fog) == J3dFogResult::Unsupported);
    assert(!build_model_fog(fog, modelFog));
    fog.type = 2;
    fog.rangeAdjustmentEnabled = true;
    assert(classify_j3d_fog(fog) == J3dFogResult::Unsupported);

    fog.rangeAdjustmentEnabled = false;
    fog.type = 1;
    assert(classify_j3d_fog(fog) == J3dFogResult::Invalid);
    fog.type = 2;
    fog.start = std::numeric_limits<float>::quiet_NaN();
    assert(classify_j3d_fog(fog) == J3dFogResult::Invalid);
}
