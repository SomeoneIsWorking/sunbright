#include <sunbright/title_adapter/guest_stage_lighting.h>

#include <array>

namespace sb::title_adapter {
namespace {

// TLightCommon, recovered from the shipping image: the ctor at 0x80229fbc and the three getters at
// 0x80229ca0 (position), 0x80229cec (ambient colour) and 0x80229d78 (light colour).
constexpr GuestAddress LIGHT_SHININESS = 0x10;
constexpr GuestAddress LIGHT_AMBIENT_ALPHA_SCALE = 0x18;
constexpr GuestAddress LIGHT_COLOR_ALPHA_SCALE = 0x1c;
constexpr GuestAddress LIGHT_AMBIENT_BASE_INDEX = 0x20;
constexpr GuestAddress LIGHT_BASE_INDEX = 0x24;
constexpr GuestAddress LIGHT_USES_LOCAL_COLOR = 0x28;
constexpr GuestAddress LIGHT_LOCAL_AMBIENT_COLORS = 0x29;
constexpr GuestAddress LIGHT_LOCAL_COLORS = 0x31;
constexpr GuestAddress LIGHT_USES_LOCAL_POSITION = 0x41;
constexpr GuestAddress LIGHT_LOCAL_POSITIONS = 0x44;
constexpr std::int32_t LOCAL_AMBIENT_COLOR_SLOTS = 2;
constexpr std::int32_t LOCAL_COLOR_SLOTS = 4;
constexpr std::int32_t LOCAL_POSITION_SLOTS = 4;

// JDrama::TGraphics. `setLight` forms `graphics + 0xb4` and hands it straight to PSMTXMultVec, so
// the view matrix is stored inline rather than behind a pointer.
constexpr GuestAddress GRAPHICS_VIEW_MATRIX = 0xb4;
constexpr std::size_t VIEW_MATRIX_ELEMENTS = 12;

// JDrama::TLightAry and TIdxLight. The entry stride and the GX light object's position inside it
// are from the image's `mulli r0, r5, 0x6c` and `addi r3, r3, 0x24`.
constexpr GuestAddress LIGHT_ARRAY_ENTRIES = 0x10;
constexpr GuestAddress LIGHT_ARRAY_COUNT = 0x14;
constexpr GuestAddress LIGHT_ENTRY_BYTES = 0x6c;
constexpr GuestAddress LIGHT_ENTRY_POSITION = 0x10;
// GXGetLightColor (0x8035f23c) reads the packed colour word at 0x0c of the light object, which
// itself sits at 0x24 of the entry.
constexpr GuestAddress LIGHT_ENTRY_COLOR = 0x24 + 0x0c;

// JDrama::TAmbAry and TAmbColor, from `mulli r4, r4, 0x18` and `addi r4, r4, 0x14`.
constexpr GuestAddress AMBIENT_ARRAY_ENTRIES = 0x10;
constexpr GuestAddress AMBIENT_ARRAY_COUNT = 0x14;
constexpr GuestAddress AMBIENT_ENTRY_BYTES = 0x18;
constexpr GuestAddress AMBIENT_ENTRY_COLOR = 0x14;

// TLightWithDBSetManager.
constexpr GuestAddress MANAGER_EFFECT_COLOR = 0x18;
constexpr GuestAddress MANAGER_EFFECT_POSITION = 0x1c;
constexpr GuestAddress MANAGER_EFFECT_ALPHA_SCALE = 0x28;
constexpr GuestAddress MANAGER_EFFECT_ENABLED = 0x54;
constexpr GuestAddress MANAGER_EFFECT_VALID = 0x55;

constexpr float BYTE_TO_UNIT = 1.0F / 255.0F;

struct PackedColor {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 0;
};

[[nodiscard]] PackedColor unpack(std::uint32_t value) noexcept {
    return {.red = static_cast<std::uint8_t>(value >> 24U),
            .green = static_cast<std::uint8_t>(value >> 16U),
            .blue = static_cast<std::uint8_t>(value >> 8U),
            .alpha = static_cast<std::uint8_t>(value)};
}

// The image's alpha scaling, kept in its own arithmetic: the byte is widened to f32, multiplied,
// truncated toward zero by `fctiwz`, and stored back as a byte. Doing this in float rather than
// rounding differently is the difference between matching the game and nearly matching it.
[[nodiscard]] std::uint8_t scale_alpha(std::uint8_t alpha, float scale) noexcept {
    const auto scaled = static_cast<std::int32_t>(static_cast<float>(alpha) * scale);
    return static_cast<std::uint8_t>(scaled);
}

[[nodiscard]] native_render::Color to_color(PackedColor color) noexcept {
    return {static_cast<float>(color.red) * BYTE_TO_UNIT,
            static_cast<float>(color.green) * BYTE_TO_UNIT,
            static_cast<float>(color.blue) * BYTE_TO_UNIT,
            static_cast<float>(color.alpha) * BYTE_TO_UNIT};
}

[[nodiscard]] bool read_vector(const GuestReader& reader, GuestAddress address,
                               native_render::Vec3& value) {
    return reader.real(address, value.x) && reader.real(address + 4, value.y) &&
           reader.real(address + 8, value.z);
}

// Resolves one of the scene's group arrays: the pointer, its entry array and its count. The count
// exists in the object but the game's getters do not consult it, so a slot past the end is a real
// out-of-range read there. Answering it as an error here is deliberate -- publishing lighting built
// from whatever followed the array would be worse than publishing none.
struct GroupArray {
    GuestAddress entries = 0;
    std::int32_t count = 0;
};

[[nodiscard]] bool read_group(const GuestReader& reader, GuestAddress pointer,
                              GuestAddress entriesField, GuestAddress countField,
                              GuestAddress& root, GroupArray& group) {
    if (!reader.word(pointer, root)) {
        return false;
    }
    if (root == 0) {
        return true;
    }
    std::uint32_t count = 0;
    if (!reader.word(root + entriesField, group.entries) ||
        !reader.word(root + countField, count)) {
        return false;
    }
    group.count = static_cast<std::int32_t>(count);
    return true;
}

} // namespace

const char* guest_stage_lighting_error_name(GuestStageLightingError error) noexcept {
    switch (error) {
    case GuestStageLightingError::None:
        return "none";
    case GuestStageLightingError::UnreadableLight:
        return "unreadable light";
    case GuestStageLightingError::UnreadableViewMatrix:
        return "unreadable view matrix";
    case GuestStageLightingError::UnreadableLightArray:
        return "unreadable light array";
    case GuestStageLightingError::NullLightArray:
        return "null light array";
    case GuestStageLightingError::NullLightEntries:
        return "null light entries";
    case GuestStageLightingError::LightIndexOutOfRange:
        return "light index out of range";
    case GuestStageLightingError::UnreadableLightEntry:
        return "unreadable light entry";
    case GuestStageLightingError::UnreadableAmbientArray:
        return "unreadable ambient array";
    case GuestStageLightingError::NullAmbientArray:
        return "null ambient array";
    case GuestStageLightingError::NullAmbientEntries:
        return "null ambient entries";
    case GuestStageLightingError::AmbientIndexOutOfRange:
        return "ambient index out of range";
    case GuestStageLightingError::UnreadableAmbientEntry:
        return "unreadable ambient entry";
    case GuestStageLightingError::UnreadableLightManager:
        return "unreadable light manager";
    case GuestStageLightingError::UnreadableEffectLight:
        return "unreadable effect light";
    }
    return "unknown";
}

GuestStageLightingError read_guest_stage_lighting(const GuestMemory& memory, GuestAddress light,
                                                  GuestAddress graphics, std::int32_t index,
                                                  const GuestStageLightingAddresses& addresses,
                                                  native_render::J3dStageLightingInput& out,
                                                  GuestStageLighting& info) noexcept {
    const GuestReader reader(memory);
    native_render::J3dStageLightingInput input{};
    GuestStageLighting read{};

    std::uint8_t usesLocalColor = 0;
    std::uint8_t usesLocalPosition = 0;
    std::uint32_t ambientBase = 0;
    std::uint32_t lightBase = 0;
    float ambientAlphaScale = 0.0F;
    float colorAlphaScale = 0.0F;
    if (!reader.real(light + LIGHT_SHININESS, input.shininess) ||
        !reader.real(light + LIGHT_AMBIENT_ALPHA_SCALE, ambientAlphaScale) ||
        !reader.real(light + LIGHT_COLOR_ALPHA_SCALE, colorAlphaScale) ||
        !reader.word(light + LIGHT_AMBIENT_BASE_INDEX, ambientBase) ||
        !reader.word(light + LIGHT_BASE_INDEX, lightBase) ||
        !reader.byte(light + LIGHT_USES_LOCAL_COLOR, usesLocalColor) ||
        !reader.byte(light + LIGHT_USES_LOCAL_POSITION, usesLocalPosition)) {
        return GuestStageLightingError::UnreadableLight;
    }
    read.usedLocalColor = usesLocalColor != 0;
    read.usedLocalPosition = usesLocalPosition != 0;

    for (std::size_t element = 0; element < VIEW_MATRIX_ELEMENTS; ++element) {
        if (!reader.real(graphics + GRAPHICS_VIEW_MATRIX + static_cast<GuestAddress>(element) * 4U,
                         input.view.value[element])) {
            return GuestStageLightingError::UnreadableViewMatrix;
        }
    }

    // The light getters are called with `index * 2`; the ambient getter with `index` itself.
    const std::int32_t lightIndex = index * 2;

    if (read.usedLocalPosition) {
        const std::int32_t slot = lightIndex >= LOCAL_POSITION_SLOTS ? 0 : lightIndex;
        read.lightSlot = static_cast<std::uint32_t>(slot);
        if (!read_vector(reader,
                         light + LIGHT_LOCAL_POSITIONS + static_cast<GuestAddress>(slot) * 12U,
                         input.primaryWorldPosition)) {
            return GuestStageLightingError::UnreadableLight;
        }
    }
    if (read.usedLocalColor) {
        const std::int32_t slot = lightIndex >= LOCAL_COLOR_SLOTS ? 0 : lightIndex;
        std::uint32_t packed = 0;
        if (!reader.word(light + LIGHT_LOCAL_COLORS + static_cast<GuestAddress>(slot) * 4U,
                         packed)) {
            return GuestStageLightingError::UnreadableLight;
        }
        input.primaryColor = to_color(unpack(packed));

        const std::int32_t ambientSlot = index >= LOCAL_AMBIENT_COLOR_SLOTS ? 0 : index;
        read.ambientSlot = static_cast<std::uint32_t>(ambientSlot);
        std::uint32_t packedAmbient = 0;
        if (!reader.word(light + LIGHT_LOCAL_AMBIENT_COLORS +
                             static_cast<GuestAddress>(ambientSlot) * 4U,
                         packedAmbient)) {
            return GuestStageLightingError::UnreadableLight;
        }
        input.ambientColor = to_color(unpack(packedAmbient));
    }

    if (!read.usedLocalPosition || !read.usedLocalColor) {
        GuestAddress root = 0;
        GroupArray lights{};
        if (!read_group(reader, addresses.lightArrayPointer, LIGHT_ARRAY_ENTRIES, LIGHT_ARRAY_COUNT,
                        root, lights)) {
            return GuestStageLightingError::UnreadableLightArray;
        }
        if (root == 0) {
            return GuestStageLightingError::NullLightArray;
        }
        if (lights.entries == 0) {
            return GuestStageLightingError::NullLightEntries;
        }
        const auto slot =
            static_cast<std::int64_t>(lightIndex) + static_cast<std::int64_t>(lightBase);
        read.lightCount = lights.count;
        if (slot < 0 || slot >= lights.count) {
            read.lightSlot = static_cast<std::uint32_t>(slot);
            return GuestStageLightingError::LightIndexOutOfRange;
        }
        read.lightSlot = static_cast<std::uint32_t>(slot);
        const GuestAddress entry =
            lights.entries + static_cast<GuestAddress>(slot) * LIGHT_ENTRY_BYTES;
        if (!read.usedLocalPosition &&
            !read_vector(reader, entry + LIGHT_ENTRY_POSITION, input.primaryWorldPosition)) {
            return GuestStageLightingError::UnreadableLightEntry;
        }
        if (!read.usedLocalColor) {
            std::uint32_t packed = 0;
            if (!reader.word(entry + LIGHT_ENTRY_COLOR, packed)) {
                return GuestStageLightingError::UnreadableLightEntry;
            }
            PackedColor color = unpack(packed);
            color.alpha = scale_alpha(color.alpha, colorAlphaScale);
            input.primaryColor = to_color(color);
        }
    }

    if (!read.usedLocalColor) {
        GuestAddress root = 0;
        GroupArray ambient{};
        if (!read_group(reader, addresses.ambientArrayPointer, AMBIENT_ARRAY_ENTRIES,
                        AMBIENT_ARRAY_COUNT, root, ambient)) {
            return GuestStageLightingError::UnreadableAmbientArray;
        }
        if (root == 0) {
            return GuestStageLightingError::NullAmbientArray;
        }
        if (ambient.entries == 0) {
            return GuestStageLightingError::NullAmbientEntries;
        }
        const auto slot = static_cast<std::int64_t>(index) + static_cast<std::int64_t>(ambientBase);
        read.ambientCount = ambient.count;
        read.ambientSlot = static_cast<std::uint32_t>(slot);
        if (slot < 0 || slot >= ambient.count) {
            return GuestStageLightingError::AmbientIndexOutOfRange;
        }
        std::uint32_t packed = 0;
        if (!reader.word(ambient.entries + static_cast<GuestAddress>(slot) * AMBIENT_ENTRY_BYTES +
                             AMBIENT_ENTRY_COLOR,
                         packed)) {
            return GuestStageLightingError::UnreadableAmbientEntry;
        }
        PackedColor color = unpack(packed);
        color.alpha = scale_alpha(color.alpha, ambientAlphaScale);
        input.ambientColor = to_color(color);
    }

    GuestAddress manager = 0;
    if (!reader.word(addresses.lightManagerPointer, manager)) {
        return GuestStageLightingError::UnreadableLightManager;
    }
    if (manager != 0) {
        std::uint8_t enabled = 0;
        std::uint8_t valid = 0;
        if (!reader.byte(manager + MANAGER_EFFECT_ENABLED, enabled) ||
            !reader.byte(manager + MANAGER_EFFECT_VALID, valid)) {
            return GuestStageLightingError::UnreadableEffectLight;
        }
        input.effectEnabled = enabled != 0 && valid != 0;
    }
    read.effectEnabled = input.effectEnabled;
    if (input.effectEnabled) {
        std::uint32_t packed = 0;
        float alphaScale = 0.0F;
        if (!read_vector(reader, manager + MANAGER_EFFECT_POSITION, input.effectWorldPosition) ||
            !reader.word(manager + MANAGER_EFFECT_COLOR, packed) ||
            !reader.real(manager + MANAGER_EFFECT_ALPHA_SCALE, alphaScale)) {
            return GuestStageLightingError::UnreadableEffectLight;
        }
        PackedColor color = unpack(packed);
        color.alpha = scale_alpha(color.alpha, alphaScale);
        input.effectColor = to_color(color);
    }

    out = input;
    info = read;
    return GuestStageLightingError::None;
}

} // namespace sb::title_adapter
