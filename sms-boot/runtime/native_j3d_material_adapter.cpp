#include "native_j3d_material_adapter.h"

#include <sunbright/native_render/j3d_alpha_masked_material.h>
#include <sunbright/native_render/j3d_stage_lighting.h>
#include <sunbright/native_render/j3d_tinted_layered_material.h>

#include <JSystem/J3D/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DTexture.hpp>
#include <JSystem/ResTIMG.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace sb {
namespace {

bool read_native_memory(native_render::ByteAddress address, std::span<std::uint8_t> output, void*) {
    const std::uint8_t* source = address.native_pointer();
    if (source == nullptr || output.empty())
        return false;
    std::memcpy(output.data(), source, output.size());
    return true;
}

std::uint32_t pack_rgba8(const J3DGXColor& color) noexcept {
    return static_cast<std::uint32_t>(color.color.r) << 24U |
           static_cast<std::uint32_t>(color.color.g) << 16U |
           static_cast<std::uint32_t>(color.color.b) << 8U | color.color.a;
}

std::uint32_t pack_rgba8(const GXColor& color) noexcept {
    return static_cast<std::uint32_t>(color.r) << 24U | static_cast<std::uint32_t>(color.g) << 16U |
           static_cast<std::uint32_t>(color.b) << 8U | color.a;
}

native_render::ResTimgDescriptor describe(const ResTIMG& image) noexcept {
    return {.format = image.format,
            .hasAlpha = image.alphaEnabled != 0,
            .width = image.width,
            .height = image.height,
            .wrapS = image.wrapS,
            .wrapT = image.wrapT,
            .paletteFormat = image.colorFormat,
            .paletteEntries = image.numColors,
            .paletteOffset = static_cast<std::int32_t>(image.paletteOffset),
            .minFilter = image.minFilter,
            .magFilter = image.magFilter,
            .imageOffset = static_cast<std::int32_t>(image.imageDataOffset)};
}

NativeJ3dMaterialResult decode_texture(J3DTexture& textureTable, std::uint16_t textureNumber,
                                       native_render::DecodedTexture& texture,
                                       native_render::ResTimgDecodeError& textureError) noexcept {
    if (textureNumber >= textureTable.getNum())
        return NativeJ3dMaterialResult::MissingTexture;
    ResTIMG* image = textureTable.getResTIMG(textureNumber);
    if (image == nullptr)
        return NativeJ3dMaterialResult::MissingTexture;
    const native_render::ByteAddress address = native_render::ByteAddress::native(image);
    textureError =
        native_render::decode_res_timg({read_native_memory, nullptr}, describe(*image), address,
                                       reinterpret_cast<std::uintptr_t>(image), texture);
    return textureError == native_render::ResTimgDecodeError::None
               ? NativeJ3dMaterialResult::Success
               : NativeJ3dMaterialResult::TextureDecodeFailure;
}

} // namespace

bool capture_native_j3d_material_state(J3DMaterial& material, bool hasVertexColor, bool hasNormal,
                                       native_render::J3dMaterialState& state) noexcept {
    J3DColorBlock* color = material.getColorBlock();
    J3DTexGenBlock* textureGeneration = material.getTexGenBlock();
    J3DTevBlock* tev = material.getTevBlock();
    J3DPEBlock* pixelEngine = material.getPEBlock();
    if (color == nullptr || textureGeneration == nullptr || tev == nullptr ||
        pixelEngine == nullptr)
        return false;

    native_render::J3dMaterialState captured{};
    const std::uint32_t colorType = color->getType();
    captured.supportedColorBlock = colorType == static_cast<std::uint32_t>('CLOF') ||
                                   colorType == static_cast<std::uint32_t>('CLON');
    captured.usesMaterialAmbient = colorType == static_cast<std::uint32_t>('CLON');
    captured.cullMode = color->getCullMode();
    captured.colorChannelCount = color->getColorChanNum();
    if (captured.colorChannelCount != 0) {
        J3DColorChan* channel = color->getColorChan(0);
        if (channel == nullptr)
            return false;
        captured.colorChannelControl = channel->mChanCtrl;
        captured.lightingEnabled = (channel->mChanCtrl & 0x0002U) != 0;
        J3DColorChan* alphaChannel = color->getColorChan(1);
        if (alphaChannel == nullptr)
            return false;
        captured.alphaChannelControl = alphaChannel->mChanCtrl;
        if (captured.colorChannelCount > 1) {
            J3DColorChan* secondColorChannel = color->getColorChan(2);
            J3DColorChan* secondAlphaChannel = color->getColorChan(3);
            if (secondColorChannel == nullptr || secondAlphaChannel == nullptr)
                return false;
            captured.colorChannelControl1 = secondColorChannel->mChanCtrl;
            captured.alphaChannelControl1 = secondAlphaChannel->mChanCtrl;
        }
    }
    J3DGXColor* materialColor = color->getMatColor(0);
    if (materialColor == nullptr)
        return false;
    captured.materialColorRgba8 = pack_rgba8(*materialColor);
    if (captured.colorChannelCount > 1) {
        J3DGXColor* secondMaterialColor = color->getMatColor(1);
        if (secondMaterialColor == nullptr)
            return false;
        captured.materialColor1Rgba8 = pack_rgba8(*secondMaterialColor);
    }
    if (colorType == static_cast<std::uint32_t>('CLON')) {
        J3DGXColor* ambientColor = color->getAmbColor(0);
        if (ambientColor == nullptr)
            return false;
        captured.ambientColorRgba8 = pack_rgba8(*ambientColor);
        if (captured.colorChannelCount > 1) {
            J3DGXColor* secondAmbientColor = color->getAmbColor(1);
            if (secondAmbientColor == nullptr)
                return false;
            captured.ambientColor1Rgba8 = pack_rgba8(*secondAmbientColor);
        }
    }
    captured.textureCoordinateCount =
        textureGeneration->getType() == static_cast<std::uint32_t>('TGBC')
            ? textureGeneration->getTexGenNum()
            : std::numeric_limits<std::uint32_t>::max();

    captured.tevBlockType = tev->getType();
    captured.pixelEngineBlockType = pixelEngine->getType();
    if (captured.pixelEngineBlockType == static_cast<std::uint32_t>('PEFL')) {
        J3DAlphaComp* alpha = pixelEngine->getAlphaComp();
        J3DBlend* blend = pixelEngine->getBlend();
        J3DZMode* depth = pixelEngine->getZMode();
        if (alpha == nullptr || blend == nullptr || depth == nullptr)
            return false;
        const J3DFog* fog = pixelEngine->getFog();
        if (fog != nullptr) {
            captured.fog = {
                .type = fog->mType,
                .rangeAdjustmentEnabled = fog->mAdjEnable != 0,
                .center = fog->mCenter,
                .start = fog->mStartZ,
                .end = fog->mEndZ,
                .near = fog->mNearZ,
                .far = fog->mFarZ,
                .colorRgba8 = pack_rgba8(fog->mColor),
            };
            std::copy_n(fog->mFogAdjTable, captured.fog.rangeAdjustmentTable.size(),
                        captured.fog.rangeAdjustmentTable.begin());
        }
        if (alpha->mAlphaCmpID != 0xFFFFU && depth->mZModeID != 0xFFFFU) {
            captured.hasExplicitPixelPolicy = true;
            captured.alphaCompare0 = static_cast<std::uint8_t>(alpha->getComp0());
            captured.alphaReference0 = alpha->getRef0();
            captured.alphaOperation = static_cast<std::uint8_t>(alpha->getOp());
            captured.alphaCompare1 = static_cast<std::uint8_t>(alpha->getComp1());
            captured.alphaReference1 = alpha->getRef1();
            captured.blendMode = blend->mBlendMode;
            captured.blendSourceFactor = blend->mSrcFactor;
            captured.blendDestinationFactor = blend->mDstFactor;
            captured.blendLogicOperation = blend->mLogicOp;
            captured.depthTest = depth->getCompareEnable() != 0;
            captured.depthCompare = depth->getFunc();
            captured.depthWrite = depth->getUpdateEnable() != 0;
        }
    }
    captured.supportedTevBlock = captured.tevBlockType == static_cast<std::uint32_t>('TVB1') ||
                                 captured.tevBlockType == static_cast<std::uint32_t>('TVB2') ||
                                 captured.tevBlockType == static_cast<std::uint32_t>('TVB4') ||
                                 captured.tevBlockType == static_cast<std::uint32_t>('TV16');
    captured.tevStageCount = tev->getTevStageNum();
    std::size_t textureBindingCount = 0;
    std::size_t stageCapacity = 0;
    if (captured.tevBlockType == static_cast<std::uint32_t>('TVB1')) {
        textureBindingCount = 1;
        stageCapacity = 1;
    } else if (captured.tevBlockType == static_cast<std::uint32_t>('TVB2')) {
        textureBindingCount = 2;
        stageCapacity = 2;
    } else if (captured.tevBlockType == static_cast<std::uint32_t>('TVB4')) {
        textureBindingCount = 4;
        stageCapacity = 4;
    } else if (captured.tevBlockType == static_cast<std::uint32_t>('TV16')) {
        textureBindingCount = 8;
        stageCapacity = 16;
    }
    if (captured.tevStageCount > stageCapacity)
        return false;
    captured.hasVertexColor = hasVertexColor;
    captured.hasNormal = hasNormal;
    if (captured.supportedTevBlock) {
        for (std::size_t colorIndex = 0; colorIndex < captured.tevColorsS10.size(); ++colorIndex) {
            J3DGXColorS10* tevColor = tev->getTevColor(colorIndex);
            if (tevColor == nullptr)
                return false;
            captured.tevColorsS10[colorIndex] = {tevColor->color.r, tevColor->color.g,
                                                 tevColor->color.b, tevColor->color.a};
        }
        captured.hasTevColors = true;
        for (std::size_t bindingIndex = 0; bindingIndex < textureBindingCount; ++bindingIndex)
            captured.textureBindings[bindingIndex].textureNumber = tev->getTexNo(bindingIndex);
        for (std::size_t stageIndex = 0; stageIndex < captured.tevStageCount; ++stageIndex) {
            J3DTevOrder* order = tev->getTevOrder(stageIndex);
            J3DTevStage* stage = tev->getTevStage(stageIndex);
            if (order == nullptr || stage == nullptr)
                return false;
            native_render::J3dTevStageState& capturedStage = captured.tevStages[stageIndex];
            capturedStage.textureCoordinate = order->mTexCoord;
            capturedStage.textureMap = order->mTexMap;
            capturedStage.colorChannel = order->mColorChan;
            static_assert(sizeof(*stage) == sizeof(std::array<std::uint8_t, 8>));
            std::memcpy(capturedStage.program.data(), stage, capturedStage.program.size());
        }
        if (captured.tevBlockType != static_cast<std::uint32_t>('TVB1')) {
            for (std::size_t colorIndex = 0; colorIndex < captured.konstColorRgba8.size();
                 ++colorIndex) {
                J3DGXColor* konstColor = tev->getTevKColor(colorIndex);
                if (konstColor == nullptr)
                    return false;
                captured.konstColorRgba8[colorIndex] = pack_rgba8(*konstColor);
            }
            for (std::size_t stageIndex = 0; stageIndex < stageCapacity; ++stageIndex) {
                captured.tevStages[stageIndex].konstColorSelection =
                    tev->getTevKColorSel(stageIndex);
                captured.tevStages[stageIndex].konstAlphaSelection =
                    tev->getTevKAlphaSel(stageIndex);
            }
        }
    }
    state = captured;
    return true;
}

NativeJ3dMaterialResult
capture_native_j3d_material(J3DMaterial& material, J3DTexture* textureTable, bool hasVertexColor,
                            bool hasNormal, CapturedNativeJ3dMaterial& captured,
                            native_render::ResTimgDecodeError& textureError) noexcept {
    native_render::J3dMaterialState state{};
    if (!capture_native_j3d_material_state(material, hasVertexColor, hasNormal, state))
        return NativeJ3dMaterialResult::InvalidInput;

    CapturedNativeJ3dMaterial result{};
    if (!native_render::build_model_fog(state.fog, result.fog))
        return NativeJ3dMaterialResult::UnsupportedProgram;
    native_render::UnlitColorMaterial colorMaterial{};
    if (native_render::classify_j3d_unlit_material(state, colorMaterial) ==
        native_render::J3dUnlitMaterialResult::Success) {
        result.material = colorMaterial;
        captured = std::move(result);
        return NativeJ3dMaterialResult::Success;
    }

    const native_render::ModelLightingContext* lighting =
        native_render::current_j3d_stage_lighting();
    native_render::LitColorMaterial litColorMaterial{};
    if (lighting != nullptr &&
        native_render::classify_j3d_lit_color_material(state, *lighting, litColorMaterial) ==
            native_render::J3dLitColorResult::Success) {
        result.material = litColorMaterial;
        captured = std::move(result);
        return NativeJ3dMaterialResult::Success;
    }

    native_render::LitSpecularColorMaterial specularColorMaterial{};
    if (lighting != nullptr && native_render::classify_j3d_specular_color_material(
                                   state, *lighting, specularColorMaterial) ==
                                   native_render::J3dSpecularColorResult::Success) {
        result.material = specularColorMaterial;
        captured = std::move(result);
        return NativeJ3dMaterialResult::Success;
    }

    const native_render::PictureTexture placeholder{.resource = 1, .width = 1, .height = 1};
    native_render::UnlitTexturedMaterial texturedMaterial{};
    const bool isUnlitTextured =
        native_render::classify_j3d_unlit_textured_material(state, placeholder, texturedMaterial) ==
        native_render::J3dUnlitTexturedResult::Success;
    native_render::AlphaMaskedColorMaterial alphaMaskedMaterial{};
    const bool isAlphaMasked = native_render::classify_j3d_alpha_masked_material(
                                   state, placeholder, alphaMaskedMaterial) ==
                               native_render::J3dAlphaMaskedMaterialResult::Success;
    native_render::LitTexturedMaterial litMaterial{};
    const bool isLitTextured =
        lighting != nullptr && native_render::classify_j3d_lit_textured_material(
                                   state, placeholder, *lighting, litMaterial) ==
                                   native_render::J3dLitTexturedResult::Success;
    native_render::LitTexturedAlphaMaskMaterial litAlphaMaskMaterial{};
    const bool isLitAlphaMask =
        lighting != nullptr &&
        native_render::classify_j3d_lit_alpha_mask_material(state, placeholder, placeholder,
                                                            *lighting, litAlphaMaskMaterial) ==
            native_render::J3dLitAlphaMaskResult::Success;
    native_render::LitLayeredTexturedMaterial layeredMaterial{};
    const bool isLayered =
        lighting != nullptr && native_render::classify_j3d_layered_material(
                                   state, placeholder, placeholder, *lighting, layeredMaterial) ==
                                   native_render::J3dLayeredMaterialResult::Success;
    native_render::LitTintedLayeredSpecularMaterial tintedLayeredMaterial{};
    const bool isTintedLayered =
        lighting != nullptr &&
        native_render::classify_j3d_tinted_layered_material(state, placeholder, placeholder,
                                                            *lighting, tintedLayeredMaterial) ==
            native_render::J3dTintedLayeredMaterialResult::Success;
    native_render::LitSpecularTexturedMaterial specularMaterial{};
    const bool isSpecularTextured =
        lighting != nullptr && native_render::classify_j3d_specular_textured_material(
                                   state, placeholder, *lighting, specularMaterial) ==
                                   native_render::J3dSpecularTexturedResult::Success;
    if (!isUnlitTextured && !isAlphaMasked && !isLitTextured && !isLitAlphaMask && !isLayered &&
        !isTintedLayered && !isSpecularTextured) {
        return NativeJ3dMaterialResult::UnsupportedProgram;
    }
    if (textureTable == nullptr)
        return NativeJ3dMaterialResult::MissingTexture;
    const std::uint16_t firstTextureNumber =
        isUnlitTextured
            ? native_render::j3d_texture_number_for_map(state, state.tevStages[0].textureMap)
            : state.textureBindings[0].textureNumber;
    const NativeJ3dMaterialResult firstTexture =
        decode_texture(*textureTable, firstTextureNumber, result.textures[0], textureError);
    if (firstTexture != NativeJ3dMaterialResult::Success)
        return firstTexture;
    result.textureCount = 1;
    if (isLitAlphaMask || isLayered || isTintedLayered) {
        const NativeJ3dMaterialResult secondTexture =
            decode_texture(*textureTable, state.textureBindings[1].textureNumber,
                           result.textures[1], textureError);
        if (secondTexture != NativeJ3dMaterialResult::Success)
            return secondTexture;
        result.textureCount = 2;
    }
    if (isAlphaMasked) {
        if (native_render::classify_j3d_alpha_masked_material(state, result.textures[0].texture,
                                                              alphaMaskedMaterial) !=
            native_render::J3dAlphaMaskedMaterialResult::Success) {
            return NativeJ3dMaterialResult::UnsupportedProgram;
        }
        result.material = alphaMaskedMaterial;
    } else if (isSpecularTextured) {
        if (native_render::classify_j3d_specular_textured_material(
                state, result.textures[0].texture, *lighting, specularMaterial) !=
            native_render::J3dSpecularTexturedResult::Success) {
            return NativeJ3dMaterialResult::UnsupportedProgram;
        }
        result.material = specularMaterial;
    } else if (isLitAlphaMask) {
        if (native_render::classify_j3d_lit_alpha_mask_material(
                state, result.textures[0].texture, result.textures[1].texture, *lighting,
                litAlphaMaskMaterial) != native_render::J3dLitAlphaMaskResult::Success) {
            return NativeJ3dMaterialResult::UnsupportedProgram;
        }
        result.material = litAlphaMaskMaterial;
    } else if (isLayered) {
        if (native_render::classify_j3d_layered_material(
                state, result.textures[0].texture, result.textures[1].texture, *lighting,
                layeredMaterial) != native_render::J3dLayeredMaterialResult::Success) {
            return NativeJ3dMaterialResult::UnsupportedProgram;
        }
        result.material = layeredMaterial;
    } else if (isTintedLayered) {
        if (native_render::classify_j3d_tinted_layered_material(
                state, result.textures[0].texture, result.textures[1].texture, *lighting,
                tintedLayeredMaterial) != native_render::J3dTintedLayeredMaterialResult::Success) {
            return NativeJ3dMaterialResult::UnsupportedProgram;
        }
        result.material = tintedLayeredMaterial;
    } else if (isLitTextured) {
        if (native_render::classify_j3d_lit_textured_material(state, result.textures[0].texture,
                                                              *lighting, litMaterial) !=
            native_render::J3dLitTexturedResult::Success) {
            return NativeJ3dMaterialResult::UnsupportedProgram;
        }
        result.material = litMaterial;
    } else {
        if (native_render::classify_j3d_unlit_textured_material(state, result.textures[0].texture,
                                                                texturedMaterial) !=
            native_render::J3dUnlitTexturedResult::Success) {
            return NativeJ3dMaterialResult::UnsupportedProgram;
        }
        result.material = texturedMaterial;
    }
    captured = std::move(result);
    return NativeJ3dMaterialResult::Success;
}

} // namespace sb
