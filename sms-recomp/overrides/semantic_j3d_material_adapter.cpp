#include "semantic_j3d_material_adapter.h"

#include <limits>

namespace sb::recomp {
namespace {

constexpr std::uint32_t kMaterialColorBlock = 0x20;
constexpr std::uint32_t kMaterialTextureGenerationBlock = 0x24;
constexpr std::uint32_t kMaterialTevBlock = 0x28;

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

} // namespace

bool capture_guest_j3d_material_state(const GuestByteReader& byteReader, std::uint32_t material,
                                      bool hasVertexColor,
                                      native_render::J3dUnlitMaterialState& state) noexcept {
    if (material == 0)
        return false;
    const BigEndianGuestReader reader(byteReader);
    std::uint32_t colorBlock = 0;
    std::uint32_t textureGenerationBlock = 0;
    std::uint32_t tevBlock = 0;
    if (!reader.u32(material + kMaterialColorBlock, colorBlock) ||
        !reader.u32(material + kMaterialTextureGenerationBlock, textureGenerationBlock) ||
        !reader.u32(material + kMaterialTevBlock, tevBlock) || colorBlock == 0 ||
        textureGenerationBlock == 0 || tevBlock == 0) {
        return false;
    }

    native_render::J3dUnlitMaterialState captured{};
    std::uint32_t colorVptr = 0;
    std::uint32_t textureGenerationVptr = 0;
    std::uint32_t tevVptr = 0;
    if (!reader.u32(colorBlock, colorVptr) ||
        !reader.u32(colorBlock + 0x04, captured.materialColorRgba8) ||
        !reader.u32(textureGenerationBlock, textureGenerationVptr) ||
        !reader.u32(tevBlock, tevVptr)) {
        return false;
    }
    std::uint32_t channelCountOffset = 0;
    std::uint32_t channelControlOffset = 0;
    if (colorVptr == kColorBlockLightOffVptr) {
        channelCountOffset = 0x0C;
        channelControlOffset = 0x0E;
    } else if (colorVptr == kColorBlockLightOnVptr) {
        channelCountOffset = 0x14;
        channelControlOffset = 0x16;
    }
    captured.supportedColorBlock = channelCountOffset != 0;
    if (captured.supportedColorBlock &&
        (!reader.u8(colorBlock + channelCountOffset, captured.colorChannelCount) ||
         !reader.u16(colorBlock + channelControlOffset, captured.colorChannelControl))) {
        return false;
    }
    captured.lightingEnabled = (captured.colorChannelControl & 0x0002U) != 0;
    captured.textureCoordinateCount = std::numeric_limits<std::uint32_t>::max();
    if (textureGenerationVptr == kTextureGenerationBlockBasicVptr &&
        !reader.u32(textureGenerationBlock + 0x04, captured.textureCoordinateCount)) {
        return false;
    }
    captured.tevBlockType = tev_type(tevVptr);
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
    if (captured.supportedTevBlock) {
        if (!reader.u16(tevBlock + 0x04, captured.textureNumber0) ||
            !reader.u8(tevBlock + orderOffset, captured.textureCoordinate0) ||
            !reader.u8(tevBlock + orderOffset + 1, captured.textureMap0) ||
            !reader.u8(tevBlock + orderOffset + 2, captured.colorChannel0) ||
            !reader.bytes(tevBlock + stageOffset, captured.tevStage0.data(),
                          captured.tevStage0.size())) {
            return false;
        }
    }
    state = captured;
    return true;
}

} // namespace sb::recomp
