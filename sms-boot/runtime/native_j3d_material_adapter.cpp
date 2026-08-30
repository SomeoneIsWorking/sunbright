#include "native_j3d_material_adapter.h"

#include <JSystem/J3D/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DTexture.hpp>
#include <JSystem/ResTIMG.hpp>

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

} // namespace

bool capture_native_j3d_material_state(J3DMaterial& material, bool hasVertexColor,
                                       native_render::J3dUnlitMaterialState& state) noexcept {
    J3DColorBlock* color = material.getColorBlock();
    J3DTexGenBlock* textureGeneration = material.getTexGenBlock();
    J3DTevBlock* tev = material.getTevBlock();
    J3DPEBlock* pixelEngine = material.getPEBlock();
    if (color == nullptr || textureGeneration == nullptr || tev == nullptr ||
        pixelEngine == nullptr)
        return false;

    native_render::J3dUnlitMaterialState captured{};
    const std::uint32_t colorType = color->getType();
    captured.supportedColorBlock = colorType == static_cast<std::uint32_t>('CLOF') ||
                                   colorType == static_cast<std::uint32_t>('CLON');
    captured.cullMode = color->getCullMode();
    captured.colorChannelCount = color->getColorChanNum();
    if (captured.colorChannelCount != 0) {
        J3DColorChan* channel = color->getColorChan(0);
        if (channel == nullptr)
            return false;
        captured.colorChannelControl = channel->mChanCtrl;
        captured.lightingEnabled = (channel->mChanCtrl & 0x0002U) != 0;
    }
    J3DGXColor* materialColor = color->getMatColor(0);
    if (materialColor == nullptr)
        return false;
    captured.materialColorRgba8 = pack_rgba8(*materialColor);
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
        captured.fogEnabled = fog != nullptr && fog->mType != 0;
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
    captured.hasVertexColor = hasVertexColor;
    if (captured.supportedTevBlock) {
        captured.textureNumber0 = tev->getTexNo(0);
        J3DTevOrder* order = tev->getTevOrder(0);
        J3DTevStage* stage = tev->getTevStage(0);
        if (order == nullptr || stage == nullptr)
            return false;
        captured.textureCoordinate0 = order->mTexCoord;
        captured.textureMap0 = order->mTexMap;
        captured.colorChannel0 = order->mColorChan;
        static_assert(sizeof(*stage) == captured.tevStage0.size());
        std::memcpy(captured.tevStage0.data(), stage, captured.tevStage0.size());
    }
    state = captured;
    return true;
}

NativeJ3dMaterialResult
capture_native_j3d_material(J3DMaterial& material, J3DTexture* textureTable, bool hasVertexColor,
                            CapturedNativeJ3dMaterial& captured,
                            native_render::ResTimgDecodeError& textureError) noexcept {
    native_render::J3dUnlitMaterialState state{};
    if (!capture_native_j3d_material_state(material, hasVertexColor, state))
        return NativeJ3dMaterialResult::InvalidInput;

    CapturedNativeJ3dMaterial result{};
    native_render::UnlitColorMaterial colorMaterial{};
    if (native_render::classify_j3d_unlit_material(state, colorMaterial) ==
        native_render::J3dUnlitMaterialResult::Success) {
        result.material = colorMaterial;
        captured = std::move(result);
        return NativeJ3dMaterialResult::Success;
    }

    const native_render::PictureTexture placeholder{.resource = 1, .width = 1, .height = 1};
    native_render::UnlitTexturedMaterial texturedMaterial{};
    if (native_render::classify_j3d_unlit_textured_material(state, placeholder, texturedMaterial) !=
        native_render::J3dUnlitTexturedResult::Success) {
        return NativeJ3dMaterialResult::UnsupportedProgram;
    }
    if (textureTable == nullptr || state.textureNumber0 >= textureTable->getNum())
        return NativeJ3dMaterialResult::MissingTexture;

    ResTIMG* image = textureTable->getResTIMG(state.textureNumber0);
    if (image == nullptr)
        return NativeJ3dMaterialResult::MissingTexture;
    const native_render::ByteAddress address = native_render::ByteAddress::native(image);
    textureError =
        native_render::decode_res_timg({read_native_memory, nullptr}, describe(*image), address,
                                       reinterpret_cast<std::uintptr_t>(image), result.texture);
    if (textureError != native_render::ResTimgDecodeError::None)
        return NativeJ3dMaterialResult::TextureDecodeFailure;
    if (native_render::classify_j3d_unlit_textured_material(state, result.texture.texture,
                                                            texturedMaterial) !=
        native_render::J3dUnlitTexturedResult::Success) {
        return NativeJ3dMaterialResult::UnsupportedProgram;
    }
    result.material = texturedMaterial;
    result.hasTexture = true;
    captured = std::move(result);
    return NativeJ3dMaterialResult::Success;
}

} // namespace sb
