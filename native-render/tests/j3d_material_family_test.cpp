#include <sunbright/native_render/j3d_lit_material.h>
#include <sunbright/native_render/j3d_material_family.h>

#include <sunbright/native_render/j3d_stage_lighting.h>

#include <cassert>
#include <cstdint>
#include <string_view>
#include <vector>

namespace {

using namespace sb::native_render;

// A texture source the test drives. It records every number it was asked for, which is how the
// "which slot names the first texture" rule is checked: the answer is otherwise invisible, since
// both slots decode to a texture the classifier accepts.
struct RecordingTextures {
    std::vector<std::uint16_t> requested{};
    std::uint16_t refuse = 0xFFFF;
    ResTimgDecodeError failWith = ResTimgDecodeError::None;

    static bool resolve(std::uint16_t textureNumber, DecodedTexture& texture,
                        ResTimgDecodeError& error, void* context) {
        auto& self = *static_cast<RecordingTextures*>(context);
        self.requested.push_back(textureNumber);
        if (textureNumber == self.refuse) {
            return false;
        }
        if (self.failWith != ResTimgDecodeError::None) {
            error = self.failWith;
            return false;
        }
        texture.texture = {.resource = textureNumber + 1U, .width = 16, .height = 8};
        texture.rgba8.assign(static_cast<std::size_t>(16 * 8 * 4), 0);
        return true;
    }
};

J3dMaterialState unlit_color_state() {
    return {
        .supportedColorBlock = true,
        .cullMode = 2,
        .colorChannelCount = 1,
        .materialColorRgba8 = 0x804020FFU,
        .tevBlockType = 0x54564231U,
        .supportedTevBlock = true,
        .tevStageCount = 1,
        .tevStages = {j3d_tev_stage(0xFF, 0xFF, 4,
                                    {0xC0, 0x40, 0xAF, 0xF0, 0xC1, 0x08, 0xBF, 0x80})},
        .pixelEngineBlockType = 0x50454F50U,
    };
}

// The same program the unlit-textured classifier accepts. It binds through texture map 1
// deliberately: slot 0 is explicitly unbound, so a first-texture number taken from slot 0 rather
// than from the stage's map answers 0xFFFF and the case fails instead of coincidentally agreeing.
J3dMaterialState unlit_textured_state() {
    J3dMaterialState state = unlit_color_state();
    state.textureCoordinateCount = 2;
    state.textureBindings[0] = j3d_texture_binding(0xFFFF);
    state.textureBindings[1] = j3d_texture_binding(7);
    state.tevStages[0] = j3d_tev_stage(1, 1, 4, {0xC0, 0x08, 0xF8, 0xAF, 0xC1, 0x08, 0xF2, 0xF0});
    return state;
}

J3dMaterialState lit_textured_state() {
    return {
        .supportedColorBlock = true,
        .usesMaterialAmbient = true,
        .cullMode = 2,
        .lightingEnabled = true,
        .colorChannelCount = 1,
        .colorChannelControl = 0x070E,
        .alphaChannelControl = 0x0700,
        .materialColorRgba8 = 0x804020FF,
        .ambientColorRgba8 = 0x102030FF,
        .textureCoordinateCount = 1,
        .tevBlockType = 0x54564231U,
        .supportedTevBlock = true,
        .tevStageCount = 1,
        .textureBindings = {j3d_texture_binding(3)},
        .tevStages = {j3d_tev_stage(0, 0, 4, {0xC0, 0x08, 0xF8, 0xAF, 0xC1, 0x08, 0xF2, 0xF0})},
        .pixelEngineBlockType = 0x50454F50U,
        .hasNormal = true,
    };
}

ModelLightingContext stage_lighting() {
    return build_j3d_stage_lighting({
        .view = {.value = {1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30}},
        .primaryWorldPosition = {1, 2, 3},
        .primaryColor = {1, 0.5F, 0.25F, 1},
        .shininess = 50.0F,
        .ambientColor = {0.2F, 0.3F, 0.4F, 1},
        .effectEnabled = true,
        .effectWorldPosition = {4, 5, 6},
        .effectColor = {0.25F, 0.5F, 1, 1},
    });
}

// Every result and every family must have its own name. A switch that fell through to "unknown",
// or two enumerators sharing a spelling, would make a per-family histogram silently wrong.
void names_every_result_and_family() {
    const J3dMaterialFamilyResult results[] = {
        J3dMaterialFamilyResult::Success,
        J3dMaterialFamilyResult::UnsupportedFog,
        J3dMaterialFamilyResult::UnsupportedProgram,
        J3dMaterialFamilyResult::NoTextureSource,
        J3dMaterialFamilyResult::MissingTexture,
        J3dMaterialFamilyResult::TextureDecodeFailure,
    };
    std::vector<std::string_view> seen{};
    for (const J3dMaterialFamilyResult result : results) {
        const std::string_view name = j3d_material_family_result_name(result);
        assert(name != std::string_view("unknown"));
        for (const std::string_view other : seen) {
            assert(other != name);
        }
        seen.push_back(name);
    }

    seen.clear();
    for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(J3dMaterialFamily::LitMaskedToon);
         ++raw) {
        const std::string_view name = j3d_material_family_name(static_cast<J3dMaterialFamily>(raw));
        assert(name != std::string_view("unknown"));
        for (const std::string_view other : seen) {
            assert(other != name);
        }
        seen.push_back(name);
    }
}

// An untextured family is classified without ever consulting the texture source, including when
// there is none. This is the case the guest probe meets first, before any texture table exists.
void untextured_needs_no_texture() {
    RecordingTextures textures{};
    ClassifiedJ3dMaterial classified{};
    assert(classify_j3d_material(unlit_color_state(), nullptr, {}, classified, nullptr) ==
           J3dMaterialFamilyResult::Success);
    assert(classified.family == J3dMaterialFamily::UnlitColor);
    assert(classified.textureCount == 0);
    assert(std::holds_alternative<UnlitColorMaterial>(classified.material));

    ClassifiedJ3dMaterial withSource{};
    assert(classify_j3d_material(unlit_color_state(), nullptr,
                                 {RecordingTextures::resolve, &textures}, withSource,
                                 nullptr) == J3dMaterialFamilyResult::Success);
    assert(textures.requested.empty());
}

void textured_decodes_the_bound_texture() {
    RecordingTextures textures{};
    ClassifiedJ3dMaterial classified{};
    assert(classify_j3d_material(unlit_textured_state(), nullptr,
                                 {RecordingTextures::resolve, &textures}, classified,
                                 nullptr) == J3dMaterialFamilyResult::Success);
    assert(classified.family == J3dMaterialFamily::UnlitTextured);
    assert(classified.textureCount == 1);
    // Named through the stage's texture map (slot 1), not through binding slot 0, which this
    // state leaves explicitly unbound.
    assert(textures.requested.size() == 1);
    assert(textures.requested[0] == 7);
    const auto& material = std::get<UnlitTexturedMaterial>(classified.material);
    assert(material.texture.resource == 8);
}

// The three ways a textured family can fail to get its pixels are reported apart, because they
// mean different things: no consumer wired a source, the consumer's table does not hold that
// number, and the bytes would not decode.
void names_each_texture_failure() {
    ClassifiedJ3dMaterial classified{};
    assert(classify_j3d_material(unlit_textured_state(), nullptr, {}, classified, nullptr) ==
           J3dMaterialFamilyResult::NoTextureSource);

    RecordingTextures refusing{.refuse = 7};
    assert(classify_j3d_material(unlit_textured_state(), nullptr,
                                 {RecordingTextures::resolve, &refusing}, classified,
                                 nullptr) == J3dMaterialFamilyResult::MissingTexture);

    RecordingTextures failing{.failWith = ResTimgDecodeError::UnsupportedFormat};
    assert(classify_j3d_material(unlit_textured_state(), nullptr,
                                 {RecordingTextures::resolve, &failing}, classified,
                                 nullptr) == J3dMaterialFamilyResult::TextureDecodeFailure);
}

// Without a published stage lighting no lit family may match. The same state must classify as a
// lit family once lighting exists, or this case would pass for a state nothing ever lit.
void lighting_gates_the_lit_families() {
    const ModelLightingContext lighting = stage_lighting();
    RecordingTextures textures{};
    ClassifiedJ3dMaterial lit{};
    assert(classify_j3d_material(lit_textured_state(), &lighting,
                                 {RecordingTextures::resolve, &textures}, lit,
                                 nullptr) == J3dMaterialFamilyResult::Success);
    assert(lit.family == J3dMaterialFamily::LitTextured);
    assert(lit.textureCount == 1);
    assert(textures.requested.size() == 1 && textures.requested[0] == 3);

    RecordingTextures unlitTextures{};
    ClassifiedJ3dMaterial unlit{};
    const J3dMaterialFamilyResult result =
        classify_j3d_material(lit_textured_state(), nullptr,
                              {RecordingTextures::resolve, &unlitTextures}, unlit, nullptr);
    assert(result != J3dMaterialFamilyResult::Success ||
           unlit.family == J3dMaterialFamily::UnlitColor ||
           unlit.family == J3dMaterialFamily::UnlitTextured ||
           unlit.family == J3dMaterialFamily::AlphaMaskedColor);
}

// The fog contract is checked before any family, so an unsupported fog refuses the material rather
// than producing one the renderer would draw without it.
void unsupported_fog_refuses_before_classifying() {
    J3dMaterialState state = unlit_color_state();
    state.fog.type = 0xFF;
    ClassifiedJ3dMaterial classified{};
    ModelFog probe{};
    if (!build_model_fog(state.fog, probe)) {
        assert(classify_j3d_material(state, nullptr, {}, classified, nullptr) ==
               J3dMaterialFamilyResult::UnsupportedFog);
        assert(classified.family == J3dMaterialFamily::None);
    }
}

// A refusal set must name a reason for every family when nothing matched. A set that came back
// mostly empty would read as "those families had no objection", which is the opposite of true:
// they were asked and said no, or were never asked at all. Both have to be visible.
void every_family_says_why_it_refused() {
    J3dMaterialState state{};
    state.supportedColorBlock = false;
    ClassifiedJ3dMaterial classified{};
    J3dFamilyRefusals refusals{};
    assert(classify_j3d_material(state, nullptr, {}, classified, &refusals) ==
           J3dMaterialFamilyResult::UnsupportedProgram);
    assert(refusals.reason[static_cast<std::size_t>(J3dMaterialFamily::None)] == nullptr);
    for (std::size_t family = 1; family < kJ3dMaterialFamilyCount; ++family) {
        assert(refusals.reason[family] != nullptr);
    }
    // The lit families were never asked, and say that rather than borrowing an unlit gate's reason.
    const char* const unasked =
        j3d_lit_color_result_name(J3dLitColorResult::MissingLightingContext);
    assert(refusals.reason[static_cast<std::size_t>(J3dMaterialFamily::LitMaskedToon)] == unasked);
    assert(refusals.reason[static_cast<std::size_t>(J3dMaterialFamily::UnlitColor)] != unasked);

    // With lighting published, the lit families are asked for real and answer with their own gates.
    const ModelLightingContext lighting = stage_lighting();
    J3dFamilyRefusals lit{};
    assert(classify_j3d_material(state, &lighting, {}, classified, &lit) ==
           J3dMaterialFamilyResult::UnsupportedProgram);
    assert(lit.reason[static_cast<std::size_t>(J3dMaterialFamily::LitMaskedToon)] != nullptr);
    assert(lit.reason[static_cast<std::size_t>(J3dMaterialFamily::LitMaskedToon)] != unasked);

    // A family that accepts leaves its own slot empty, so a refusal set never blames a winner.
    J3dFamilyRefusals accepted{};
    assert(classify_j3d_material(unlit_color_state(), nullptr, {}, classified, &accepted) ==
           J3dMaterialFamilyResult::Success);
    assert(accepted.reason[static_cast<std::size_t>(J3dMaterialFamily::UnlitColor)] == nullptr);
}

} // namespace

int main() {
    names_every_result_and_family();
    untextured_needs_no_texture();
    textured_decodes_the_bound_texture();
    names_each_texture_failure();
    lighting_gates_the_lit_families();
    unsupported_fog_refuses_before_classifying();
    every_family_says_why_it_refused();
    return 0;
}
