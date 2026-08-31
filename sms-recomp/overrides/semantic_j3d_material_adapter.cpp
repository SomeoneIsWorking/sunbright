#include "semantic_j3d_material_adapter.h"

#include <cstddef>
#include <limits>

namespace sb::recomp {
namespace {

constexpr std::uint32_t kMaterialColorBlock = 0x20;
constexpr std::uint32_t kMaterialTextureGenerationBlock = 0x24;
constexpr std::uint32_t kMaterialTevBlock = 0x28;
constexpr std::uint32_t kMaterialPixelEngineBlock = 0x30;

// Retail US constructor stores, not PAL symbol arithmetic: createColorBlock 0x802d6b14,
// createTexGenBlock 0x802d6e88, and createTevBlock 0x802d6fc4 write these exact addresses. The
// CodeWarrior object vptr points at the table symbol itself; there is no Itanium +8 address point.
constexpr std::uint32_t kColorBlockLightOffVptr = 0x803E0D38;
constexpr std::uint32_t kColorBlockLightOnVptr = 0x803E0CD4;
constexpr std::uint32_t kTextureGenerationBlockBasicVptr = 0x803E0C84;
constexpr std::uint32_t kTevBlock1Vptr = 0x803E0BE8;
constexpr std::uint32_t kTevBlock2Vptr = 0x803E0B4C;
constexpr std::uint32_t kTevBlock4Vptr = 0x803E0AB0;
constexpr std::uint32_t kTevBlock16Vptr = 0x803E0A14;
// Retail US J3DMaterial::createPEBlock at 0x802d77a8 stores these vptrs. The full block is accepted
// only when its explicit alpha/blend/depth fields exactly match one supported common policy.
constexpr std::uint32_t kPixelEngineOpaqueVptr = 0x803E0E64;
constexpr std::uint32_t kPixelEngineTextureEdgeVptr = 0x803E0E00;
constexpr std::uint32_t kPixelEngineTranslucentVptr = 0x803E0D9C;
constexpr std::uint32_t kPixelEngineFullVptr = 0x803E0968;

std::uint32_t tev_type(std::uint32_t vptr) noexcept {
    switch (vptr) {
    case kTevBlock1Vptr:
        return 0x54564231U; // 'TVB1'
    case kTevBlock2Vptr:
        return 0x54564232U; // 'TVB2'
    case kTevBlock4Vptr:
        return 0x54564234U; // 'TVB4'
    case kTevBlock16Vptr:
        return 0x54563136U; // 'TV16'
    default:
        return 0;
    }
}

std::uint32_t pixel_engine_type(std::uint32_t vptr) noexcept {
    switch (vptr) {
    case kPixelEngineOpaqueVptr:
        return 0x50454F50U; // 'PEOP'
    case kPixelEngineTextureEdgeVptr:
        return 0x50454544U; // 'PEED'
    case kPixelEngineTranslucentVptr:
        return 0x5045584CU; // 'PEXL'
    case kPixelEngineFullVptr:
        return 0x5045464CU; // 'PEFL'
    default:
        return 0;
    }
}

bool tev_konst_offsets(std::uint32_t vptr, std::uint32_t& colorOffset,
                       std::uint32_t& colorSelectionOffset,
                       std::uint32_t& alphaSelectionOffset) noexcept {
    switch (vptr) {
    case kTevBlock2Vptr:
        colorOffset = 0x41;
        colorSelectionOffset = 0x51;
        alphaSelectionOffset = 0x53;
        return true;
    case kTevBlock4Vptr:
        colorOffset = 0x5E;
        colorSelectionOffset = 0x6E;
        alphaSelectionOffset = 0x72;
        return true;
    case kTevBlock16Vptr:
        colorOffset = 0xF6;
        colorSelectionOffset = 0x106;
        alphaSelectionOffset = 0x116;
        return true;
    default:
        return false;
    }
}

} // namespace

bool capture_guest_j3d_material_state(const GuestByteReader& byteReader, std::uint32_t material,
                                      bool hasVertexColor, bool hasNormal,
                                      native_render::J3dMaterialState& state) noexcept {
    if (material == 0)
        return false;
    const BigEndianGuestReader reader(byteReader);
    std::uint32_t colorBlock = 0;
    std::uint32_t textureGenerationBlock = 0;
    std::uint32_t tevBlock = 0;
    std::uint32_t pixelEngineBlock = 0;
    if (!reader.u32(material + kMaterialColorBlock, colorBlock) ||
        !reader.u32(material + kMaterialTextureGenerationBlock, textureGenerationBlock) ||
        !reader.u32(material + kMaterialTevBlock, tevBlock) ||
        !reader.u32(material + kMaterialPixelEngineBlock, pixelEngineBlock) || colorBlock == 0 ||
        textureGenerationBlock == 0 || tevBlock == 0 || pixelEngineBlock == 0) {
        return false;
    }

    native_render::J3dMaterialState captured{};
    std::uint32_t colorVptr = 0;
    std::uint32_t textureGenerationVptr = 0;
    std::uint32_t tevVptr = 0;
    std::uint32_t pixelEngineVptr = 0;
    if (!reader.u32(colorBlock, colorVptr) ||
        !reader.u32(colorBlock + 0x04, captured.materialColorRgba8) ||
        !reader.u32(textureGenerationBlock, textureGenerationVptr) ||
        !reader.u32(tevBlock, tevVptr) || !reader.u32(pixelEngineBlock, pixelEngineVptr)) {
        return false;
    }
    std::uint32_t channelCountOffset = 0;
    std::uint32_t channelControlOffset = 0;
    if (colorVptr == kColorBlockLightOffVptr) {
        channelCountOffset = 0x0C;
        channelControlOffset = 0x0E;
        if (!reader.u8(colorBlock + 0x16, captured.cullMode))
            return false;
    } else if (colorVptr == kColorBlockLightOnVptr) {
        channelCountOffset = 0x14;
        channelControlOffset = 0x16;
        if (!reader.u8(colorBlock + 0x40, captured.cullMode))
            return false;
    }
    captured.supportedColorBlock = channelCountOffset != 0;
    captured.usesMaterialAmbient = colorVptr == kColorBlockLightOnVptr;
    if (captured.supportedColorBlock &&
        (!reader.u8(colorBlock + channelCountOffset, captured.colorChannelCount) ||
         !reader.u16(colorBlock + channelControlOffset, captured.colorChannelControl) ||
         !reader.u16(colorBlock + channelControlOffset + 2, captured.alphaChannelControl))) {
        return false;
    }
    if (captured.colorChannelCount > 1 &&
        (!reader.u16(colorBlock + channelControlOffset + 4, captured.colorChannelControl1) ||
         !reader.u16(colorBlock + channelControlOffset + 6, captured.alphaChannelControl1) ||
         !reader.u32(colorBlock + 0x08, captured.materialColor1Rgba8))) {
        return false;
    }
    if (colorVptr == kColorBlockLightOnVptr &&
        !reader.u32(colorBlock + 0x0C, captured.ambientColorRgba8)) {
        return false;
    }
    if (colorVptr == kColorBlockLightOnVptr && captured.colorChannelCount > 1 &&
        !reader.u32(colorBlock + 0x10, captured.ambientColor1Rgba8)) {
        return false;
    }
    captured.lightingEnabled = (captured.colorChannelControl & 0x0002U) != 0;
    captured.textureCoordinateCount = std::numeric_limits<std::uint32_t>::max();
    if (textureGenerationVptr == kTextureGenerationBlockBasicVptr &&
        !reader.u32(textureGenerationBlock + 0x04, captured.textureCoordinateCount)) {
        return false;
    }
    captured.tevBlockType = tev_type(tevVptr);
    captured.pixelEngineBlockType = pixel_engine_type(pixelEngineVptr);
    if (pixelEngineVptr == kPixelEngineFullVptr) {
        std::uint32_t fog = 0;
        std::uint16_t alphaId = 0;
        std::uint16_t depthId = 0;
        if (!reader.u32(pixelEngineBlock + 0x04, fog) ||
            !reader.u16(pixelEngineBlock + 0x08, alphaId) ||
            !reader.u8(pixelEngineBlock + 0x0A, captured.alphaReference0) ||
            !reader.u8(pixelEngineBlock + 0x0B, captured.alphaReference1) ||
            !reader.u8(pixelEngineBlock + 0x0C, captured.blendMode) ||
            !reader.u8(pixelEngineBlock + 0x0D, captured.blendSourceFactor) ||
            !reader.u8(pixelEngineBlock + 0x0E, captured.blendDestinationFactor) ||
            !reader.u8(pixelEngineBlock + 0x0F, captured.blendLogicOperation) ||
            !reader.u16(pixelEngineBlock + 0x10, depthId)) {
            return false;
        }
        if (fog != 0) {
            std::uint8_t fogType = 0;
            if (!reader.u8(fog, fogType))
                return false;
            captured.fogEnabled = fogType != 0;
        }
        if (alphaId != 0xFFFFU && depthId != 0xFFFFU) {
            captured.hasExplicitPixelPolicy = true;
            captured.alphaCompare0 = static_cast<std::uint8_t>(alphaId >> 5U);
            captured.alphaOperation = static_cast<std::uint8_t>((alphaId >> 3U) & 3U);
            captured.alphaCompare1 = static_cast<std::uint8_t>(alphaId & 7U);
            captured.depthTest = (depthId & 0x10U) != 0;
            captured.depthCompare = static_cast<std::uint8_t>((depthId >> 1U) & 7U);
            captured.depthWrite = (depthId & 1U) != 0;
        }
    }
    std::uint32_t stageCountOffset = 0;
    std::uint32_t orderOffset = 0;
    std::uint32_t stageOffset = 0;
    if (tevVptr == kTevBlock1Vptr) {
        captured.tevStageCount = 1;
        orderOffset = 0x06;
        stageOffset = 0x0A;
    } else if (tevVptr == kTevBlock2Vptr) {
        stageCountOffset = 0x30;
        orderOffset = 0x08;
        stageOffset = 0x31;
    } else if (tevVptr == kTevBlock4Vptr) {
        stageCountOffset = 0x1C;
        orderOffset = 0x0C;
        stageOffset = 0x1D;
    } else if (tevVptr == kTevBlock16Vptr) {
        stageCountOffset = 0x54;
        orderOffset = 0x14;
        stageOffset = 0x55;
    }
    captured.supportedTevBlock = orderOffset != 0;
    if (stageCountOffset != 0 && !reader.u8(tevBlock + stageCountOffset, captured.tevStageCount)) {
        return false;
    }
    captured.hasVertexColor = hasVertexColor;
    captured.hasNormal = hasNormal;
    if (captured.supportedTevBlock) {
        if (!reader.u16(tevBlock + 0x04, captured.textureNumber0) ||
            !reader.u8(tevBlock + orderOffset, captured.textureCoordinate0) ||
            !reader.u8(tevBlock + orderOffset + 1, captured.textureMap0) ||
            !reader.u8(tevBlock + orderOffset + 2, captured.colorChannel0) ||
            !reader.bytes(tevBlock + stageOffset, captured.tevStage0.data(),
                          captured.tevStage0.size())) {
            return false;
        }
        if (captured.tevStageCount >= 2 &&
            (!reader.u16(tevBlock + 0x06, captured.textureNumber1) ||
             !reader.u8(tevBlock + orderOffset + 4, captured.textureCoordinate1) ||
             !reader.u8(tevBlock + orderOffset + 5, captured.textureMap1) ||
             !reader.u8(tevBlock + orderOffset + 6, captured.colorChannel1) ||
             !reader.bytes(tevBlock + stageOffset + 8, captured.tevStage1.data(),
                           captured.tevStage1.size()))) {
            return false;
        }
        if (captured.tevStageCount >= 2) {
            std::uint32_t colorOffset = 0;
            std::uint32_t colorSelectionOffset = 0;
            std::uint32_t alphaSelectionOffset = 0;
            if (!tev_konst_offsets(tevVptr, colorOffset, colorSelectionOffset,
                                   alphaSelectionOffset)) {
                return false;
            }
            for (std::size_t colorIndex = 0; colorIndex < captured.konstColorRgba8.size();
                 ++colorIndex) {
                if (!reader.u32(tevBlock + colorOffset + static_cast<std::uint32_t>(colorIndex * 4),
                                captured.konstColorRgba8[colorIndex])) {
                    return false;
                }
            }
            if (!reader.u8(tevBlock + colorSelectionOffset, captured.konstColorSelection0) ||
                !reader.u8(tevBlock + colorSelectionOffset + 1, captured.konstColorSelection1) ||
                !reader.u8(tevBlock + alphaSelectionOffset, captured.konstAlphaSelection0) ||
                !reader.u8(tevBlock + alphaSelectionOffset + 1, captured.konstAlphaSelection1)) {
                return false;
            }
        }
    }
    state = captured;
    return true;
}

} // namespace sb::recomp
