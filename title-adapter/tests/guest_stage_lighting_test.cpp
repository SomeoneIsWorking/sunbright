#include <sunbright/title_adapter/guest_stage_lighting.h>

#include "guest_image.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

namespace {

using namespace sb::title_adapter;
using sb::title_adapter::test::Image;
using sb::title_adapter::test::RAM_BASE;
using sb::title_adapter::test::read_image;

constexpr GuestAddress LIGHT = RAM_BASE + 0x100;
constexpr GuestAddress GRAPHICS = RAM_BASE + 0x200;
constexpr GuestAddress LIGHT_ARRAY_POINTER = RAM_BASE + 0x400;
constexpr GuestAddress AMBIENT_ARRAY_POINTER = RAM_BASE + 0x404;
constexpr GuestAddress MANAGER_POINTER = RAM_BASE + 0x408;
constexpr GuestAddress LIGHT_ARRAY = RAM_BASE + 0x410;
constexpr GuestAddress LIGHT_ENTRIES = RAM_BASE + 0x440;
constexpr GuestAddress AMBIENT_ARRAY = RAM_BASE + 0x600;
constexpr GuestAddress AMBIENT_ENTRIES = RAM_BASE + 0x630;
constexpr GuestAddress MANAGER = RAM_BASE + 0x700;

constexpr GuestStageLightingAddresses ADDRESSES{
    .lightArrayPointer = LIGHT_ARRAY_POINTER,
    .ambientArrayPointer = AMBIENT_ARRAY_POINTER,
    .lightManagerPointer = MANAGER_POINTER,
};

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0005F;
}

// An identity view with a translation, so a position that was transformed is distinguishable from
// one that was copied.
void write_view(Image& image) {
    const float view[12] = {1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30};
    for (int element = 0; element < 12; ++element) {
        image.real(GRAPHICS + 0xb4 + static_cast<GuestAddress>(element) * 4, view[element]);
    }
}

// A scene whose lights come from the group arrays: the ordinary path, and the one where the two
// separate alpha scales at 0x18 and 0x1c are observable.
Image group_scene() {
    Image image{};
    write_view(image);
    image.real(LIGHT + 0x10, 50.0F); // shininess
    image.real(LIGHT + 0x18, 0.5F);  // ambient alpha scale
    image.real(LIGHT + 0x1c, 0.25F); // light-colour alpha scale
    image.word(LIGHT + 0x20, 1);     // ambient base index
    image.word(LIGHT + 0x24, 2);     // light base index
    image.byte(LIGHT + 0x28, 0);     // no local colour
    image.byte(LIGHT + 0x41, 0);     // no local position

    image.word(LIGHT_ARRAY_POINTER, LIGHT_ARRAY);
    image.word(LIGHT_ARRAY + 0x10, LIGHT_ENTRIES);
    image.word(LIGHT_ARRAY + 0x14, 6);
    // Slot 2 (index 0 * 2 + base 2). Position at 0x10, packed colour at 0x24 + 0x0c.
    const GuestAddress entry = LIGHT_ENTRIES + 2 * 0x6c;
    image.real(entry + 0x10, 1.0F);
    image.real(entry + 0x14, 2.0F);
    image.real(entry + 0x18, 3.0F);
    image.word(entry + 0x30, 0x4080C0FFU);

    image.word(AMBIENT_ARRAY_POINTER, AMBIENT_ARRAY);
    image.word(AMBIENT_ARRAY + 0x10, AMBIENT_ENTRIES);
    image.word(AMBIENT_ARRAY + 0x14, 4);
    // Slot 1 (index 0 + base 1), colour at 0x14 of a 0x18-byte entry.
    image.word(AMBIENT_ENTRIES + 1 * 0x18 + 0x14, 0x20406080U);

    image.word(MANAGER_POINTER, MANAGER);
    image.byte(MANAGER + 0x54, 0);
    image.byte(MANAGER + 0x55, 0);
    return image;
}

void names_every_error() {
    const GuestStageLightingError errors[] = {
        GuestStageLightingError::None,
        GuestStageLightingError::UnreadableLight,
        GuestStageLightingError::UnreadableViewMatrix,
        GuestStageLightingError::UnreadableLightArray,
        GuestStageLightingError::NullLightArray,
        GuestStageLightingError::NullLightEntries,
        GuestStageLightingError::LightIndexOutOfRange,
        GuestStageLightingError::UnreadableLightEntry,
        GuestStageLightingError::UnreadableAmbientArray,
        GuestStageLightingError::NullAmbientArray,
        GuestStageLightingError::NullAmbientEntries,
        GuestStageLightingError::AmbientIndexOutOfRange,
        GuestStageLightingError::UnreadableAmbientEntry,
        GuestStageLightingError::UnreadableLightManager,
        GuestStageLightingError::UnreadableEffectLight,
    };
    std::vector<std::string_view> seen{};
    for (const GuestStageLightingError error : errors) {
        const std::string_view name = guest_stage_lighting_error_name(error);
        assert(name != std::string_view("unknown"));
        for (const std::string_view other : seen) {
            assert(other != name);
        }
        seen.push_back(name);
    }
}

void reads_the_group_arrays() {
    Image image = group_scene();
    const GuestMemory memory{read_image, &image};
    sb::native_render::J3dStageLightingInput input{};
    GuestStageLighting info{};
    assert(read_guest_stage_lighting(memory, LIGHT, GRAPHICS, 0, ADDRESSES, input, info) ==
           GuestStageLightingError::None);

    assert(!info.usedLocalColor && !info.usedLocalPosition);
    assert(info.lightSlot == 2 && info.lightCount == 6);
    assert(info.ambientSlot == 1 && info.ambientCount == 4);
    assert(!info.effectEnabled);

    assert(near(input.shininess, 50.0F));
    assert(near(input.primaryWorldPosition.x, 1.0F));
    assert(near(input.primaryWorldPosition.z, 3.0F));
    // The view matrix is published untransformed; build_j3d_stage_lighting applies it.
    assert(near(input.view.value[3], 10.0F));

    // 0x4080C0FF with the light-colour scale 0.25 -> alpha 0xFF * 0.25 truncates to 63.
    assert(near(input.primaryColor.r, 0x40 / 255.0F));
    assert(near(input.primaryColor.g, 0x80 / 255.0F));
    assert(near(input.primaryColor.b, 0xC0 / 255.0F));
    assert(near(input.primaryColor.a, 63.0F / 255.0F));

    // 0x20406080 with the AMBIENT scale 0.5 -> alpha 0x80 * 0.5 = 64. Reading this scale from
    // 0x1c instead, as the decomp header does, would give 0x80 * 0.25 = 32.
    assert(near(input.ambientColor.r, 0x20 / 255.0F));
    assert(near(input.ambientColor.a, 64.0F / 255.0F));
}

// The two overrides are independent: the position comes from the light object while the colours
// still come from the group array, or the reverse.
void reads_the_local_overrides() {
    Image image = group_scene();
    image.byte(LIGHT + 0x41, 1);
    image.real(LIGHT + 0x44, 7.0F);
    image.real(LIGHT + 0x48, 8.0F);
    image.real(LIGHT + 0x4c, 9.0F);
    const GuestMemory memory{read_image, &image};
    sb::native_render::J3dStageLightingInput input{};
    GuestStageLighting info{};
    assert(read_guest_stage_lighting(memory, LIGHT, GRAPHICS, 0, ADDRESSES, input, info) ==
           GuestStageLightingError::None);
    assert(info.usedLocalPosition && !info.usedLocalColor);
    assert(near(input.primaryWorldPosition.x, 7.0F));
    assert(near(input.primaryWorldPosition.z, 9.0F));
    // Still the group colour, scaled by the group path's own scale.
    assert(near(input.primaryColor.a, 63.0F / 255.0F));

    Image localColor = group_scene();
    localColor.byte(LIGHT + 0x28, 1);
    localColor.word(LIGHT + 0x31, 0x11223344U); // local light colour slot 0
    localColor.word(LIGHT + 0x29, 0x55667788U); // local ambient colour slot 0
    const GuestMemory colorMemory{read_image, &localColor};
    sb::native_render::J3dStageLightingInput colorInput{};
    GuestStageLighting colorInfo{};
    assert(read_guest_stage_lighting(colorMemory, LIGHT, GRAPHICS, 0, ADDRESSES, colorInput,
                                     colorInfo) == GuestStageLightingError::None);
    assert(colorInfo.usedLocalColor);
    // The local path does not scale the alpha: the image returns the stored bytes unchanged.
    assert(near(colorInput.primaryColor.a, 0x44 / 255.0F));
    assert(near(colorInput.ambientColor.a, 0x88 / 255.0F));
}

void reads_the_effect_light() {
    Image image = group_scene();
    image.byte(MANAGER + 0x54, 1);
    image.byte(MANAGER + 0x55, 1);
    image.word(MANAGER + 0x18, 0x102030C0U);
    image.real(MANAGER + 0x1c, 4.0F);
    image.real(MANAGER + 0x20, 5.0F);
    image.real(MANAGER + 0x24, 6.0F);
    image.real(MANAGER + 0x28, 0.5F);
    const GuestMemory memory{read_image, &image};
    sb::native_render::J3dStageLightingInput input{};
    GuestStageLighting info{};
    assert(read_guest_stage_lighting(memory, LIGHT, GRAPHICS, 0, ADDRESSES, input, info) ==
           GuestStageLightingError::None);
    assert(info.effectEnabled && input.effectEnabled);
    assert(near(input.effectWorldPosition.y, 5.0F));
    assert(near(input.effectColor.a, 96.0F / 255.0F)); // 0xC0 * 0.5

    // Both gates must be set: mEffectEnabled alone is not enough.
    Image half = group_scene();
    half.byte(MANAGER + 0x54, 1);
    half.byte(MANAGER + 0x55, 0);
    const GuestMemory halfMemory{read_image, &half};
    sb::native_render::J3dStageLightingInput halfInput{};
    GuestStageLighting halfInfo{};
    assert(read_guest_stage_lighting(halfMemory, LIGHT, GRAPHICS, 0, ADDRESSES, halfInput,
                                     halfInfo) == GuestStageLightingError::None);
    assert(!halfInput.effectEnabled);
}

// The light getters are called with index * 2 and the ambient getter with the index itself. A
// reader that doubled both, or neither, lands on a different slot in at least one of them.
void doubles_only_the_light_index() {
    Image image = group_scene();
    // index 1 -> light slot 1*2 + base 2 = 4, ambient slot 1 + base 1 = 2.
    const GuestAddress entry = LIGHT_ENTRIES + 4 * 0x6c;
    image.real(entry + 0x10, 41.0F);
    image.word(entry + 0x30, 0xFFFFFFFFU);
    image.word(AMBIENT_ENTRIES + 2 * 0x18 + 0x14, 0xFFFFFF40U);
    const GuestMemory memory{read_image, &image};
    sb::native_render::J3dStageLightingInput input{};
    GuestStageLighting info{};
    assert(read_guest_stage_lighting(memory, LIGHT, GRAPHICS, 1, ADDRESSES, input, info) ==
           GuestStageLightingError::None);
    assert(info.lightSlot == 4);
    assert(info.ambientSlot == 2);
    assert(near(input.primaryWorldPosition.x, 41.0F));
    assert(near(input.ambientColor.a, 32.0F / 255.0F)); // 0x40 * 0.5
}

// Every refusal leaves the caller's input untouched, so a scene that half-read cannot publish a
// light rig built partly from this scene and partly from the last one.
void refuses_what_it_cannot_read() {
    const sb::native_render::J3dStageLightingInput untouched{};
    const auto check = [&untouched](Image& image, GuestStageLightingError expected) {
        const GuestMemory memory{read_image, &image};
        sb::native_render::J3dStageLightingInput input{};
        GuestStageLighting info{};
        assert(read_guest_stage_lighting(memory, LIGHT, GRAPHICS, 0, ADDRESSES, input, info) ==
               expected);
        assert(input.shininess == untouched.shininess);
        assert(input.primaryWorldPosition == untouched.primaryWorldPosition);
        assert(input.primaryColor == untouched.primaryColor);
        assert(input.ambientColor == untouched.ambientColor);
    };

    Image nullLights = group_scene();
    nullLights.word(LIGHT_ARRAY_POINTER, 0);
    check(nullLights, GuestStageLightingError::NullLightArray);

    Image nullEntries = group_scene();
    nullEntries.word(LIGHT_ARRAY + 0x10, 0);
    check(nullEntries, GuestStageLightingError::NullLightEntries);

    Image shortLights = group_scene();
    shortLights.word(LIGHT_ARRAY + 0x14, 2); // slot 2 is now past the end
    check(shortLights, GuestStageLightingError::LightIndexOutOfRange);

    Image nullAmbient = group_scene();
    nullAmbient.word(AMBIENT_ARRAY_POINTER, 0);
    check(nullAmbient, GuestStageLightingError::NullAmbientArray);

    Image shortAmbient = group_scene();
    shortAmbient.word(AMBIENT_ARRAY + 0x14, 1); // slot 1 is now past the end
    check(shortAmbient, GuestStageLightingError::AmbientIndexOutOfRange);

    Image unmapped = group_scene();
    const GuestMemory memory{read_image, &unmapped};
    sb::native_render::J3dStageLightingInput input{};
    GuestStageLighting info{};
    assert(read_guest_stage_lighting(memory, 0x10000000, GRAPHICS, 0, ADDRESSES, input, info) ==
           GuestStageLightingError::UnreadableLight);
    assert(read_guest_stage_lighting(memory, LIGHT, 0x10000000, 0, ADDRESSES, input, info) ==
           GuestStageLightingError::UnreadableViewMatrix);
}

} // namespace

int main() {
    names_every_error();
    reads_the_group_arrays();
    reads_the_local_overrides();
    reads_the_effect_light();
    doubles_only_the_light_index();
    refuses_what_it_cannot_read();
    return 0;
}
