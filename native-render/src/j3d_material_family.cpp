#include <sunbright/native_render/j3d_material_family.h>

#include <sunbright/native_render/j3d_alpha_masked_material.h>
#include <sunbright/native_render/j3d_dual_alpha_effect_material.h>
#include <sunbright/native_render/j3d_layered_material.h>
#include <sunbright/native_render/j3d_lit_alpha_mask_material.h>
#include <sunbright/native_render/j3d_lit_alpha_tint_material.h>
#include <sunbright/native_render/j3d_lit_material.h>
#include <sunbright/native_render/j3d_masked_toon_material.h>
#include <sunbright/native_render/j3d_specular_material.h>
#include <sunbright/native_render/j3d_tinted_layered_material.h>
#include <sunbright/native_render/j3d_unlit_material.h>

#include <utility>

namespace sb::native_render {
namespace {

// A stand-in used only to ask each textured classifier whether the *program* is its family. The
// textures themselves cannot be decoded before that answer, because how many to decode depends on
// which family matched.
constexpr PictureTexture PLACEHOLDER{.resource = 1, .width = 1, .height = 1};

// The untextured families, in the order the renderer resolves them.
[[nodiscard]] J3dMaterialFamily classify_untextured(const J3dMaterialState& state,
                                                    const ModelLightingContext* lighting,
                                                    ModelMaterial& material) noexcept {
    UnlitColorMaterial unlitColor{};
    if (classify_j3d_unlit_material(state, unlitColor) == J3dUnlitMaterialResult::Success) {
        material = unlitColor;
        return J3dMaterialFamily::UnlitColor;
    }
    if (lighting == nullptr) {
        return J3dMaterialFamily::None;
    }
    LitColorMaterial litColor{};
    if (classify_j3d_lit_color_material(state, *lighting, litColor) == J3dLitColorResult::Success) {
        material = litColor;
        return J3dMaterialFamily::LitColor;
    }
    LitSpecularRampMaterial specularRamp{};
    if (classify_j3d_specular_ramp_material(state, *lighting, specularRamp) ==
        J3dSpecularRampResult::Success) {
        material = specularRamp;
        return J3dMaterialFamily::LitSpecularRamp;
    }
    LitSpecularColorMaterial specularColor{};
    if (classify_j3d_specular_color_material(state, *lighting, specularColor) ==
        J3dSpecularColorResult::Success) {
        material = specularColor;
        return J3dMaterialFamily::LitSpecularColor;
    }
    return J3dMaterialFamily::None;
}

// Which of the textured families accept this program. They are asked independently rather than in
// order because two of the three answers taken from them differ: the winning family is the first
// match in priority order, but the number of textures to decode and which number names the first
// one are taken from the whole match set. Keeping that explicit is the point -- collapsing it to
// "ask in order, stop at the first yes" would silently change what is decoded wherever two exact
// families accept the same program.
struct TexturedMatches {
    bool dualAlphaEffect = false;
    bool alphaMasked = false;
    bool specularTextured = false;
    bool litAlphaMask = false;
    bool litAlphaTint = false;
    bool layered = false;
    bool tintedLayered = false;
    bool maskedToon = false;
    bool litTextured = false;
    bool unlitTextured = false;

    [[nodiscard]] J3dMaterialFamily family() const noexcept {
        if (dualAlphaEffect) {
            return J3dMaterialFamily::LitDualAlphaEffect;
        }
        if (alphaMasked) {
            return J3dMaterialFamily::AlphaMaskedColor;
        }
        if (specularTextured) {
            return J3dMaterialFamily::LitSpecularTextured;
        }
        if (litAlphaMask) {
            return J3dMaterialFamily::LitTexturedAlphaMask;
        }
        if (litAlphaTint) {
            return J3dMaterialFamily::LitAlphaTint;
        }
        if (layered) {
            return J3dMaterialFamily::LitLayeredTextured;
        }
        if (tintedLayered) {
            return J3dMaterialFamily::LitTintedLayeredSpecular;
        }
        if (maskedToon) {
            return J3dMaterialFamily::LitMaskedToon;
        }
        if (litTextured) {
            return J3dMaterialFamily::LitTextured;
        }
        if (unlitTextured) {
            return J3dMaterialFamily::UnlitTextured;
        }
        return J3dMaterialFamily::None;
    }

    [[nodiscard]] std::size_t textureCount() const noexcept {
        if (maskedToon) {
            return 4;
        }
        if (dualAlphaEffect || litAlphaMask || layered || tintedLayered) {
            return 2;
        }
        return 1;
    }
};

[[nodiscard]] TexturedMatches match_textured(const J3dMaterialState& state,
                                             const ModelLightingContext* lighting) noexcept {
    TexturedMatches matches{};
    UnlitTexturedMaterial unlitTextured{};
    matches.unlitTextured =
        classify_j3d_unlit_textured_material(state, PLACEHOLDER, unlitTextured) ==
        J3dUnlitTexturedResult::Success;
    AlphaMaskedColorMaterial alphaMasked{};
    matches.alphaMasked = classify_j3d_alpha_masked_material(state, PLACEHOLDER, alphaMasked) ==
                          J3dAlphaMaskedMaterialResult::Success;
    if (lighting == nullptr) {
        return matches;
    }
    LitDualAlphaEffectMaterial dualAlphaEffect{};
    matches.dualAlphaEffect = classify_j3d_dual_alpha_effect_material(
                                  state, PLACEHOLDER, PLACEHOLDER, *lighting, dualAlphaEffect) ==
                              J3dDualAlphaEffectMaterialResult::Success;
    LitSpecularTexturedMaterial specularTextured{};
    matches.specularTextured =
        classify_j3d_specular_textured_material(state, PLACEHOLDER, *lighting, specularTextured) ==
        J3dSpecularTexturedResult::Success;
    LitTexturedAlphaMaskMaterial litAlphaMask{};
    matches.litAlphaMask =
        classify_j3d_lit_alpha_mask_material(state, PLACEHOLDER, PLACEHOLDER, *lighting,
                                             litAlphaMask) == J3dLitAlphaMaskResult::Success;
    LitAlphaTintMaterial litAlphaTint{};
    matches.litAlphaTint =
        classify_j3d_lit_alpha_tint_material(state, PLACEHOLDER, *lighting, litAlphaTint) ==
        J3dLitAlphaTintResult::Success;
    LitLayeredTexturedMaterial layered{};
    matches.layered = classify_j3d_layered_material(state, PLACEHOLDER, PLACEHOLDER, *lighting,
                                                    layered) == J3dLayeredMaterialResult::Success;
    LitTintedLayeredSpecularMaterial tintedLayered{};
    matches.tintedLayered = classify_j3d_tinted_layered_material(state, PLACEHOLDER, PLACEHOLDER,
                                                                 *lighting, tintedLayered) ==
                            J3dTintedLayeredMaterialResult::Success;
    LitMaskedToonMaterial maskedToon{};
    matches.maskedToon = classify_j3d_masked_toon_material(
                             state, PLACEHOLDER, PLACEHOLDER, PLACEHOLDER, PLACEHOLDER, *lighting,
                             maskedToon) == J3dMaskedToonMaterialResult::Success;
    LitTexturedMaterial litTextured{};
    matches.litTextured =
        classify_j3d_lit_textured_material(state, PLACEHOLDER, *lighting, litTextured) ==
        J3dLitTexturedResult::Success;
    return matches;
}

// Re-runs the selected family's classifier against the decoded textures. The placeholder pass
// answered "is this program yours"; this one produces the material, and a family that refuses its
// own real textures is an unsupported program rather than a texture failure.
[[nodiscard]] bool build_textured(const J3dMaterialState& state,
                                  const ModelLightingContext* lighting, J3dMaterialFamily family,
                                  const std::array<DecodedTexture, kMaxClassifiedTextures>& decoded,
                                  ModelMaterial& material) noexcept {
    const PictureTexture& first = decoded[0].texture;
    const PictureTexture& second = decoded[1].texture;
    switch (family) {
    case J3dMaterialFamily::LitDualAlphaEffect: {
        LitDualAlphaEffectMaterial built{};
        if (classify_j3d_dual_alpha_effect_material(state, first, second, *lighting, built) !=
            J3dDualAlphaEffectMaterialResult::Success) {
            return false;
        }
        material = built;
        return true;
    }
    case J3dMaterialFamily::AlphaMaskedColor: {
        AlphaMaskedColorMaterial built{};
        if (classify_j3d_alpha_masked_material(state, first, built) !=
            J3dAlphaMaskedMaterialResult::Success) {
            return false;
        }
        material = built;
        return true;
    }
    case J3dMaterialFamily::LitSpecularTextured: {
        LitSpecularTexturedMaterial built{};
        if (classify_j3d_specular_textured_material(state, first, *lighting, built) !=
            J3dSpecularTexturedResult::Success) {
            return false;
        }
        material = built;
        return true;
    }
    case J3dMaterialFamily::LitTexturedAlphaMask: {
        LitTexturedAlphaMaskMaterial built{};
        if (classify_j3d_lit_alpha_mask_material(state, first, second, *lighting, built) !=
            J3dLitAlphaMaskResult::Success) {
            return false;
        }
        material = built;
        return true;
    }
    case J3dMaterialFamily::LitAlphaTint: {
        LitAlphaTintMaterial built{};
        if (classify_j3d_lit_alpha_tint_material(state, first, *lighting, built) !=
            J3dLitAlphaTintResult::Success) {
            return false;
        }
        material = built;
        return true;
    }
    case J3dMaterialFamily::LitLayeredTextured: {
        LitLayeredTexturedMaterial built{};
        if (classify_j3d_layered_material(state, first, second, *lighting, built) !=
            J3dLayeredMaterialResult::Success) {
            return false;
        }
        material = built;
        return true;
    }
    case J3dMaterialFamily::LitTintedLayeredSpecular: {
        LitTintedLayeredSpecularMaterial built{};
        if (classify_j3d_tinted_layered_material(state, first, second, *lighting, built) !=
            J3dTintedLayeredMaterialResult::Success) {
            return false;
        }
        material = built;
        return true;
    }
    case J3dMaterialFamily::LitMaskedToon: {
        LitMaskedToonMaterial built{};
        if (classify_j3d_masked_toon_material(state, first, second, decoded[2].texture,
                                              decoded[3].texture, *lighting,
                                              built) != J3dMaskedToonMaterialResult::Success) {
            return false;
        }
        material = built;
        return true;
    }
    case J3dMaterialFamily::LitTextured: {
        LitTexturedMaterial built{};
        if (classify_j3d_lit_textured_material(state, first, *lighting, built) !=
            J3dLitTexturedResult::Success) {
            return false;
        }
        material = built;
        return true;
    }
    case J3dMaterialFamily::UnlitTextured: {
        UnlitTexturedMaterial built{};
        if (classify_j3d_unlit_textured_material(state, first, built) !=
            J3dUnlitTexturedResult::Success) {
            return false;
        }
        material = built;
        return true;
    }
    default:
        return false;
    }
}

} // namespace

const char* j3d_material_family_result_name(J3dMaterialFamilyResult result) noexcept {
    switch (result) {
    case J3dMaterialFamilyResult::Success:
        return "success";
    case J3dMaterialFamilyResult::UnsupportedFog:
        return "unsupported_fog";
    case J3dMaterialFamilyResult::UnsupportedProgram:
        return "unsupported_program";
    case J3dMaterialFamilyResult::NoTextureSource:
        return "no_texture_source";
    case J3dMaterialFamilyResult::MissingTexture:
        return "missing_texture";
    case J3dMaterialFamilyResult::TextureDecodeFailure:
        return "texture_decode_failure";
    }
    return "unknown";
}

const char* j3d_material_family_name(J3dMaterialFamily family) noexcept {
    switch (family) {
    case J3dMaterialFamily::None:
        return "none";
    case J3dMaterialFamily::UnlitColor:
        return "unlit_color";
    case J3dMaterialFamily::LitColor:
        return "lit_color";
    case J3dMaterialFamily::LitSpecularRamp:
        return "lit_specular_ramp";
    case J3dMaterialFamily::LitSpecularColor:
        return "lit_specular_color";
    case J3dMaterialFamily::UnlitTextured:
        return "unlit_textured";
    case J3dMaterialFamily::AlphaMaskedColor:
        return "alpha_masked_color";
    case J3dMaterialFamily::LitSpecularTextured:
        return "lit_specular_textured";
    case J3dMaterialFamily::LitTextured:
        return "lit_textured";
    case J3dMaterialFamily::LitTexturedAlphaMask:
        return "lit_textured_alpha_mask";
    case J3dMaterialFamily::LitDualAlphaEffect:
        return "lit_dual_alpha_effect";
    case J3dMaterialFamily::LitAlphaTint:
        return "lit_alpha_tint";
    case J3dMaterialFamily::LitLayeredTextured:
        return "lit_layered_textured";
    case J3dMaterialFamily::LitTintedLayeredSpecular:
        return "lit_tinted_layered_specular";
    case J3dMaterialFamily::LitMaskedToon:
        return "lit_masked_toon";
    }
    return "unknown";
}

J3dMaterialFamilyResult classify_j3d_material(const J3dMaterialState& state,
                                              const ModelLightingContext* lighting,
                                              const J3dTextureSource& textures,
                                              ClassifiedJ3dMaterial& out) noexcept {
    ClassifiedJ3dMaterial result{};
    if (!build_model_fog(state.fog, result.fog)) {
        return J3dMaterialFamilyResult::UnsupportedFog;
    }

    const J3dMaterialFamily untextured = classify_untextured(state, lighting, result.material);
    if (untextured != J3dMaterialFamily::None) {
        result.family = untextured;
        out = std::move(result);
        return J3dMaterialFamilyResult::Success;
    }

    const TexturedMatches matches = match_textured(state, lighting);
    const J3dMaterialFamily family = matches.family();
    if (family == J3dMaterialFamily::None) {
        return J3dMaterialFamilyResult::UnsupportedProgram;
    }
    if (textures.resolve == nullptr) {
        return J3dMaterialFamilyResult::NoTextureSource;
    }

    // A program the unlit-textured family accepts names its first texture through that stage's
    // texture map rather than through binding slot zero, and it does so even when a higher-priority
    // family wins the classification. Every later slot reads the bindings in order.
    const std::size_t count = matches.textureCount();
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint16_t number =
            index == 0 && matches.unlitTextured
                ? j3d_texture_number_for_map(state, state.tevStages[0].textureMap)
                : state.textureBindings[index].textureNumber;
        ResTimgDecodeError error = ResTimgDecodeError::None;
        if (!textures.resolve(number, result.textures[index], error, textures.context)) {
            return error == ResTimgDecodeError::None
                       ? J3dMaterialFamilyResult::MissingTexture
                       : J3dMaterialFamilyResult::TextureDecodeFailure;
        }
        result.textureCount = static_cast<std::uint8_t>(index + 1);
    }

    if (!build_textured(state, lighting, family, result.textures, result.material)) {
        return J3dMaterialFamilyResult::UnsupportedProgram;
    }
    result.family = family;
    out = std::move(result);
    return J3dMaterialFamilyResult::Success;
}

} // namespace sb::native_render
