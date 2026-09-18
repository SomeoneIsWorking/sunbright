#include <sunbright/title_adapter/guest_j3d_color.h>

#include <array>

namespace sb::title_adapter {
namespace {

// Retail J3DColorBlockLightOff and J3DColorBlockLightOn, from
// decomp/sms/include/JSystem/J3D/J3DGraphBase/Blocks/J3DColorBlocks.hpp. Both open with the vtable
// pointer their virtual destructor requires, so the declared members follow it. They differ by more
// than the ambient colours: every field after the material colours sits at a different offset,
// which is why the layout is chosen by class rather than shared with an ambient flag.
constexpr GuestAddress COLOR_BLOCK_VTABLE = 0x00;

struct ColorBlockLayout {
    GuestAddress materialColors;
    GuestAddress ambientColors; // 0 when the class has none.
    GuestAddress channelCount;
    GuestAddress channels;
    GuestAddress cullMode;
};

constexpr ColorBlockLayout LIGHT_OFF{.materialColors = 0x04,
                                     .ambientColors = 0,
                                     .channelCount = 0x0c,
                                     .channels = 0x0e,
                                     .cullMode = 0x16};

constexpr ColorBlockLayout LIGHT_ON{.materialColors = 0x04,
                                    .ambientColors = 0x0c,
                                    .channelCount = 0x14,
                                    .channels = 0x16,
                                    .cullMode = 0x40};

// A J3DGXColor is a GXColor: four bytes, r g b a in that order.
constexpr GuestAddress COLOR_BYTES = 4;

// A J3DColorChan is its packed control word and nothing else.
constexpr GuestAddress CHANNEL_BYTES = 2;

// The four entries are colour 0, alpha 0, colour 1, alpha 1.
constexpr std::uint32_t CHANNEL_COLOR0 = 0;
constexpr std::uint32_t CHANNEL_ALPHA0 = 1;
constexpr std::uint32_t CHANNEL_COLOR1 = 2;
constexpr std::uint32_t CHANNEL_ALPHA1 = 3;

// GX_CC_ENABLE in the packed channel control: whether the hardware lights this channel rather than
// passing the material colour straight through.
constexpr std::uint16_t CHANNEL_LIGHTING_ENABLED = 0x0002;

[[nodiscard]] bool read_color(const GuestReader& reader, GuestAddress address,
                              std::uint32_t& rgba8) {
    std::array<std::uint8_t, COLOR_BYTES> color{};
    if (!reader.bytes(address, color)) {
        return false;
    }
    rgba8 = static_cast<std::uint32_t>(color[0]) << 24U |
            static_cast<std::uint32_t>(color[1]) << 16U |
            static_cast<std::uint32_t>(color[2]) << 8U | static_cast<std::uint32_t>(color[3]);
    return true;
}

[[nodiscard]] bool read_channel(const GuestReader& reader, GuestAddress channels,
                                std::uint32_t index, std::uint16_t& control) {
    return reader.half(channels + index * CHANNEL_BYTES, control);
}

} // namespace

std::uint32_t guest_color_block_type(GuestColorBlockKind kind) noexcept {
    switch (kind) {
    case GuestColorBlockKind::LightOff:
        return 0x434c4f46; // 'CLOF'
    case GuestColorBlockKind::LightOn:
        return 0x434c4f4e; // 'CLON'
    }
    return 0;
}

const char* name(GuestColorError error) noexcept {
    switch (error) {
    case GuestColorError::None:
        return "none";
    case GuestColorError::NoReader:
        return "no reader";
    case GuestColorError::NullBlock:
        return "null block";
    case GuestColorError::UnreadableBlock:
        return "unreadable block";
    case GuestColorError::UnknownBlockKind:
        return "unknown block kind";
    case GuestColorError::UnreadableChannelCount:
        return "unreadable channel count";
    case GuestColorError::ColorChannelCountOutOfRange:
        return "colour channel count out of range";
    case GuestColorError::UnreadableChannelControl:
        return "unreadable channel control";
    case GuestColorError::UnreadableMaterialColor:
        return "unreadable material colour";
    case GuestColorError::UnreadableAmbientColor:
        return "unreadable ambient colour";
    case GuestColorError::UnreadableCullMode:
        return "unreadable cull mode";
    }
    return "unknown";
}

GuestColorError read_guest_color_block(const GuestMemory& memory, GuestAddress block,
                                       const GuestColorBlockVtables& vtables,
                                       native_render::J3dMaterialState& state,
                                       GuestColorBlock& out) noexcept {
    if (memory.read == nullptr) {
        return GuestColorError::NoReader;
    }
    if (block == 0) {
        return GuestColorError::NullBlock;
    }
    const GuestReader reader(memory);
    GuestAddress vtable = 0;
    if (!reader.word(block + COLOR_BLOCK_VTABLE, vtable)) {
        return GuestColorError::UnreadableBlock;
    }
    GuestColorBlock result{};
    ColorBlockLayout layout{};
    if (vtable == vtables.lightOff) {
        result.kind = GuestColorBlockKind::LightOff;
        layout = LIGHT_OFF;
    } else if (vtable == vtables.lightOn) {
        result.kind = GuestColorBlockKind::LightOn;
        layout = LIGHT_ON;
    } else {
        return GuestColorError::UnknownBlockKind;
    }
    result.blockType = guest_color_block_type(result.kind);

    native_render::J3dMaterialState read = state;
    read.supportedColorBlock = true;
    read.usesMaterialAmbient = result.kind == GuestColorBlockKind::LightOn;
    read.lightingEnabled = false;
    read.colorChannelControl = 0;
    read.alphaChannelControl = 0;
    read.colorChannelControl1 = 0;
    read.alphaChannelControl1 = 0;
    read.ambientColorRgba8 = 0;
    read.ambientColor1Rgba8 = 0;
    read.materialColor1Rgba8 = 0;

    std::uint8_t channelCount = 0;
    if (!reader.byte(block + layout.channelCount, channelCount)) {
        return GuestColorError::UnreadableChannelCount;
    }
    if (channelCount > kGuestMaxColorChannels) {
        return GuestColorError::ColorChannelCountOutOfRange;
    }
    read.colorChannelCount = channelCount;
    result.colorChannelCount = channelCount;

    const GuestAddress channels = block + layout.channels;
    if (channelCount != 0) {
        if (!read_channel(reader, channels, CHANNEL_COLOR0, read.colorChannelControl) ||
            !read_channel(reader, channels, CHANNEL_ALPHA0, read.alphaChannelControl)) {
            return GuestColorError::UnreadableChannelControl;
        }
        read.lightingEnabled = (read.colorChannelControl & CHANNEL_LIGHTING_ENABLED) != 0;
        if (channelCount > 1) {
            if (!read_channel(reader, channels, CHANNEL_COLOR1, read.colorChannelControl1) ||
                !read_channel(reader, channels, CHANNEL_ALPHA1, read.alphaChannelControl1)) {
                return GuestColorError::UnreadableChannelControl;
            }
        }
    }

    const GuestAddress materialColors = block + layout.materialColors;
    if (!read_color(reader, materialColors, read.materialColorRgba8)) {
        return GuestColorError::UnreadableMaterialColor;
    }
    if (channelCount > 1 &&
        !read_color(reader, materialColors + COLOR_BYTES, read.materialColor1Rgba8)) {
        return GuestColorError::UnreadableMaterialColor;
    }
    if (read.usesMaterialAmbient) {
        const GuestAddress ambientColors = block + layout.ambientColors;
        if (!read_color(reader, ambientColors, read.ambientColorRgba8)) {
            return GuestColorError::UnreadableAmbientColor;
        }
        if (channelCount > 1 &&
            !read_color(reader, ambientColors + COLOR_BYTES, read.ambientColor1Rgba8)) {
            return GuestColorError::UnreadableAmbientColor;
        }
    }
    if (!reader.byte(block + layout.cullMode, read.cullMode)) {
        return GuestColorError::UnreadableCullMode;
    }

    result.lightingEnabled = read.lightingEnabled;
    state = read;
    out = result;
    return GuestColorError::None;
}

} // namespace sb::title_adapter
