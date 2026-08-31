#include <sunbright/native_render/j3d_unlit_material.h>

namespace sb::native_render {
namespace {

constexpr std::uint8_t kColor0Alpha0 = 4;
constexpr std::array<std::uint8_t, 8> kRasterColorPassThrough{0xC0, 0x40, 0xAF, 0xF0,
                                                              0xC1, 0x08, 0xBF, 0x80};
constexpr std::array<std::uint8_t, 8> kTextureTimesRaster{0xC0, 0x08, 0xF8, 0xAF,
                                                          0xC1, 0x08, 0xF2, 0xF0};
constexpr std::uint32_t kPixelEngineOpaque = 0x50454F50U;      // 'PEOP'
constexpr std::uint32_t kPixelEngineTextureEdge = 0x50454544U; // 'PEED'
constexpr std::uint32_t kPixelEngineTranslucent = 0x5045584CU; // 'PEXL'
constexpr std::uint32_t kPixelEngineFull = 0x5045464CU;        // 'PEFL'

bool full_policy_matches(const J3dMaterialState& state, std::uint8_t alphaCompare0,
                         std::uint8_t alphaReference0, std::uint8_t alphaCompare1,
                         std::uint8_t alphaReference1, std::uint8_t blendMode,
                         std::uint8_t blendSource, std::uint8_t blendDestination,
                         bool depthWrite) noexcept {
    constexpr std::uint8_t kAlphaAnd = 0;
    constexpr std::uint8_t kAlphaOr = 1;
    constexpr std::uint8_t kAlphaXnor = 3;
    constexpr std::uint8_t kAlways = 7;
    constexpr std::uint8_t kLogicCopy = 3;
    constexpr std::uint8_t kDepthLessOrEqual = 3;
    const bool twoAlwaysComparisons = alphaCompare0 == kAlways && alphaCompare1 == kAlways;
    const bool alphaPolicyMatches =
        state.alphaCompare0 == alphaCompare0 && state.alphaCompare1 == alphaCompare1 &&
        (twoAlwaysComparisons
             ? state.alphaOperation == kAlphaAnd || state.alphaOperation == kAlphaOr ||
                   state.alphaOperation == kAlphaXnor
             : state.alphaReference0 == alphaReference0 && state.alphaOperation == kAlphaAnd &&
                   state.alphaReference1 == alphaReference1);
    return state.hasExplicitPixelPolicy && alphaPolicyMatches && state.blendMode == blendMode &&
           state.blendSourceFactor == blendSource &&
           state.blendDestinationFactor == blendDestination &&
           state.blendLogicOperation == kLogicCopy && state.depthTest &&
           state.depthCompare == kDepthLessOrEqual && state.depthWrite == depthWrite &&
           !state.fogEnabled;
}

} // namespace

const char* j3d_unlit_material_result_name(J3dUnlitMaterialResult result) noexcept {
    switch (result) {
    case J3dUnlitMaterialResult::Success:
        return "success";
    case J3dUnlitMaterialResult::UnsupportedColorBlock:
        return "unsupported colour block";
    case J3dUnlitMaterialResult::Lighting:
        return "lighting";
    case J3dUnlitMaterialResult::MissingColorChannel:
        return "missing color channel";
    case J3dUnlitMaterialResult::TextureBinding:
        return "texture binding";
    case J3dUnlitMaterialResult::UnsupportedTevBlock:
        return "unsupported colour-stage block";
    case J3dUnlitMaterialResult::MultipleTevStages:
        return "multiple active colour stages";
    case J3dUnlitMaterialResult::UnsupportedColorProgram:
        return "unsupported colour program";
    case J3dUnlitMaterialResult::MissingVertexColor:
        return "missing vertex colour";
    case J3dUnlitMaterialResult::UnsupportedRasterPolicy:
        return "unsupported raster policy";
    }
    return "unknown";
}

const char* j3d_unlit_textured_result_name(J3dUnlitTexturedResult result) noexcept {
    switch (result) {
    case J3dUnlitTexturedResult::Success:
        return "success";
    case J3dUnlitTexturedResult::UnsupportedColorBlock:
        return "unsupported colour block";
    case J3dUnlitTexturedResult::Lighting:
        return "lighting";
    case J3dUnlitTexturedResult::MissingColorChannel:
        return "missing color channel";
    case J3dUnlitTexturedResult::UnsupportedTevBlock:
        return "unsupported colour-stage block";
    case J3dUnlitTexturedResult::MultipleTevStages:
        return "multiple active colour stages";
    case J3dUnlitTexturedResult::MissingTextureCoordinate:
        return "missing texture coordinate";
    case J3dUnlitTexturedResult::UnsupportedTextureBinding:
        return "unsupported texture binding";
    case J3dUnlitTexturedResult::UnsupportedColorProgram:
        return "unsupported colour program";
    case J3dUnlitTexturedResult::MissingVertexColor:
        return "missing vertex colour";
    case J3dUnlitTexturedResult::UnsupportedRasterPolicy:
        return "unsupported raster policy";
    }
    return "unknown";
}

J3dRasterPolicyResult classify_j3d_raster_policy(const J3dMaterialState& state,
                                                 ModelRasterPolicy& policy) noexcept {
    if (state.cullMode > static_cast<std::uint8_t>(ModelCullMode::All))
        return J3dRasterPolicyResult::UnsupportedCullMode;

    ModelRasterPolicy result{};
    result.cull = static_cast<ModelCullMode>(state.cullMode);
    if (state.pixelEngineBlockType == kPixelEngineOpaque) {
        // J3DPEBlockOpa::load: pass alpha, replace colour, LEQUAL depth test + write.
    } else if (state.pixelEngineBlockType == kPixelEngineTextureEdge) {
        // J3DPEBlockTexEdge::load: alpha >= 128/255, replace colour, LEQUAL + write.
        result.alphaTest = ModelAlphaTest::GreaterOrEqualHalf;
    } else if (state.pixelEngineBlockType == kPixelEngineTranslucent) {
        // J3DPEBlockXlu::load: source-alpha blend, LEQUAL depth test without depth writes.
        result.depthWrite = false;
        result.blend = ModelBlendMode::SourceAlpha;
    } else if (state.pixelEngineBlockType == kPixelEngineFull) {
        constexpr std::uint8_t kAlways = 7;
        constexpr std::uint8_t kGreaterOrEqual = 6;
        constexpr std::uint8_t kLessOrEqual = 3;
        constexpr std::uint8_t kBlendNone = 0;
        constexpr std::uint8_t kBlend = 1;
        constexpr std::uint8_t kZero = 0;
        constexpr std::uint8_t kOne = 1;
        constexpr std::uint8_t kSourceAlpha = 4;
        constexpr std::uint8_t kInverseSourceAlpha = 5;
        if (full_policy_matches(state, kAlways, 0, kAlways, 0, kBlendNone, kOne, kZero, true)) {
            // Exact expanded form of J3DPEBlockOpa.
        } else if (full_policy_matches(state, kGreaterOrEqual, 0x80, kLessOrEqual, 0xFF, kBlendNone,
                                       kOne, kZero, true)) {
            result.alphaTest = ModelAlphaTest::GreaterOrEqualHalf;
        } else if (full_policy_matches(state, kAlways, 0, kAlways, 0, kBlend, kSourceAlpha,
                                       kInverseSourceAlpha, false)) {
            result.depthWrite = false;
            result.blend = ModelBlendMode::SourceAlpha;
        } else {
            return J3dRasterPolicyResult::UnsupportedPixelEngineBlock;
        }
    } else {
        return J3dRasterPolicyResult::UnsupportedPixelEngineBlock;
    }
    policy = result;
    return J3dRasterPolicyResult::Success;
}

J3dUnlitMaterialFeatures inspect_j3d_unlit_material(const J3dMaterialState& state) noexcept {
    const bool vertexColor = (state.colorChannelControl & 0x0001U) != 0;
    return {
        .supportedColorBlock = state.supportedColorBlock,
        .lightingEnabled = state.lightingEnabled,
        .hasColorChannel = state.colorChannelCount != 0,
        .textureBound = state.textureNumber0 != 0xFFFFU || state.textureCoordinate0 != 0xFFU ||
                        state.textureMap0 != 0xFFU,
        .supportedTevBlock = state.supportedTevBlock,
        .singleTevStage = state.tevStageCount == 1,
        .rasterColorPassThrough =
            state.colorChannel0 == kColor0Alpha0 && state.tevStage0 == kRasterColorPassThrough,
        .requiredVertexColorPresent = !vertexColor || state.hasVertexColor,
    };
}

J3dUnlitMaterialResult classify_j3d_unlit_material(const J3dMaterialState& state,
                                                   UnlitColorMaterial& material) noexcept {
    const J3dUnlitMaterialFeatures features = inspect_j3d_unlit_material(state);
    if (!features.supportedColorBlock)
        return J3dUnlitMaterialResult::UnsupportedColorBlock;
    if (features.lightingEnabled)
        return J3dUnlitMaterialResult::Lighting;
    if (!features.hasColorChannel)
        return J3dUnlitMaterialResult::MissingColorChannel;
    if (!features.supportedTevBlock)
        return J3dUnlitMaterialResult::UnsupportedTevBlock;
    if (!features.singleTevStage)
        return J3dUnlitMaterialResult::MultipleTevStages;
    if (features.textureBound)
        return J3dUnlitMaterialResult::TextureBinding;
    if (!features.rasterColorPassThrough)
        return J3dUnlitMaterialResult::UnsupportedColorProgram;
    ModelRasterPolicy raster{};
    if (classify_j3d_raster_policy(state, raster) != J3dRasterPolicyResult::Success)
        return J3dUnlitMaterialResult::UnsupportedRasterPolicy;

    const bool vertexColor = (state.colorChannelControl & 0x0001U) != 0;
    if (!features.requiredVertexColorPresent)
        return J3dUnlitMaterialResult::MissingVertexColor;
    material.usesVertexColor = vertexColor;
    material.baseColor =
        vertexColor ? Color{1.0F, 1.0F, 1.0F, 1.0F} : color_from_rgba8(state.materialColorRgba8);
    material.raster = raster;
    return J3dUnlitMaterialResult::Success;
}

J3dUnlitTexturedResult
classify_j3d_unlit_textured_material(const J3dMaterialState& state, const PictureTexture& texture,
                                     UnlitTexturedMaterial& material) noexcept {
    if (!state.supportedColorBlock)
        return J3dUnlitTexturedResult::UnsupportedColorBlock;
    if (state.lightingEnabled)
        return J3dUnlitTexturedResult::Lighting;
    if (state.colorChannelCount == 0)
        return J3dUnlitTexturedResult::MissingColorChannel;
    if (!state.supportedTevBlock)
        return J3dUnlitTexturedResult::UnsupportedTevBlock;
    if (state.tevStageCount != 1)
        return J3dUnlitTexturedResult::MultipleTevStages;
    if (state.textureCoordinateCount == 0)
        return J3dUnlitTexturedResult::MissingTextureCoordinate;
    if (state.textureNumber0 == 0xFFFFU || state.textureCoordinate0 != 0 ||
        state.textureMap0 != 0 || state.colorChannel0 != kColor0Alpha0 || texture.resource == 0 ||
        texture.width == 0 || texture.height == 0) {
        return J3dUnlitTexturedResult::UnsupportedTextureBinding;
    }
    if (state.tevStage0 != kTextureTimesRaster)
        return J3dUnlitTexturedResult::UnsupportedColorProgram;
    ModelRasterPolicy raster{};
    if (classify_j3d_raster_policy(state, raster) != J3dRasterPolicyResult::Success)
        return J3dUnlitTexturedResult::UnsupportedRasterPolicy;
    const bool vertexColor = (state.colorChannelControl & 0x0001U) != 0;
    if (vertexColor && !state.hasVertexColor)
        return J3dUnlitTexturedResult::MissingVertexColor;
    material.texture = texture;
    material.usesVertexColor = vertexColor;
    material.raster = raster;
    return J3dUnlitTexturedResult::Success;
}

} // namespace sb::native_render
