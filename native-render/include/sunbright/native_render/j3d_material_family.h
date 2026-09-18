#pragma once

#include <sunbright/native_render/image.h>
#include <sunbright/native_render/j3d_fog.h>
#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/native_render/model.h>
#include <sunbright/native_render/res_timg_decode.h>

#include <array>
#include <cstdint>
#include <span>

namespace sb::native_render {

// The one place that decides which material family a normalized J3D state belongs to.
//
// Every individual classifier answers one question -- "is this exactly my family?" -- and refuses
// anything it does not recognise. Choosing between them is a separate rule: the families overlap
// at their edges, the lit ones need a stage-lighting context, and the textured ones need their
// textures decoded before the final classification can run, which means the choice has to be made
// once on placeholder textures and confirmed once on the real ones. That rule is what lives here.
//
// It is deliberately independent of where the state came from. The runtime that normalized a
// `J3dMaterialState` -- the decomp object layout, or GMSE01's own objects read through the
// dynarec -- supplies a `J3dTextureSource` to resolve a texture number and nothing else. There is
// no second copy of this ordering for either runtime to drift from.

// How a texture number is turned into decoded pixels. `resolve` answers false when the number
// names no texture in the consumer's table; `error` distinguishes a decode failure from a missing
// texture and is left untouched when the number is simply out of range.
struct J3dTextureSource {
    bool (*resolve)(std::uint16_t textureNumber, DecodedTexture& texture, ResTimgDecodeError& error,
                    void* context) = nullptr;
    void* context = nullptr;
};

enum class J3dMaterialFamilyResult : std::uint8_t {
    Success,
    UnsupportedFog,
    UnsupportedProgram,
    NoTextureSource,
    MissingTexture,
    TextureDecodeFailure,
};

// Which family the state landed in. Reported rather than inferred from the variant so a diagnostic
// can count coverage per family, and so `None` names the refusal cases without a second signal.
enum class J3dMaterialFamily : std::uint8_t {
    None,
    UnlitColor,
    LitColor,
    LitSpecularRamp,
    LitSpecularColor,
    UnlitTextured,
    AlphaMaskedColor,
    LitSpecularTextured,
    LitTextured,
    LitTexturedAlphaMask,
    LitDualAlphaEffect,
    LitAlphaTint,
    LitLayeredTextured,
    LitTintedLayeredSpecular,
    LitMaskedToon,
    LitMaskedSpecular,
    TexturedEffect,
    UnlitTexturedEffect,
    DoubledTexturePair,
    TintedTextureSum,
    MaskedDoubledTexture,
    InterpolatedRegisters,
};

// Every family, including `None`, so a refusal set can be indexed by the family that refused. The
// enumerator named here must stay the last one: appending a family after it leaves the refusal set
// one slot short, and the new family's refusal writes past the end. `tools/structure_check.py`
// checks that this names the final enumerator so the mistake cannot be made silently again.
constexpr std::size_t kJ3dMaterialFamilyCount =
    static_cast<std::size_t>(J3dMaterialFamily::InterpolatedRegisters) + 1;

// Why each family turned a state down, indexed by `J3dMaterialFamily`. A null entry means the
// family accepted the program, and the `None` slot is always null. The strings are the families'
// own result names, so there is no second vocabulary to keep in step with theirs.
//
// A refusal set is how the runtimes answer "what would this material need in order to render",
// which is a different question from "does it render" and is the one that names the next port.
struct J3dFamilyRefusals {
    std::array<const char*, kJ3dMaterialFamilyCount> reason{};
};

// At most four textures: the masked-toon family binds the largest set.
constexpr std::size_t kMaxClassifiedTextures = 4;

struct ClassifiedJ3dMaterial {
    ModelMaterial material{};
    ModelFog fog{};
    std::array<DecodedTexture, kMaxClassifiedTextures> textures{};
    std::uint8_t textureCount = 0;
    J3dMaterialFamily family = J3dMaterialFamily::None;
};

// The classified material's textures as the frame sink takes them. Both runtimes submit the same
// views, so the mapping from a decoded texture to an image view has one owner rather than a copy
// per runtime that could disagree about which of the four slots a family filled.
[[nodiscard]] std::span<const DecodedImageView>
j3d_material_image_views(const ClassifiedJ3dMaterial& classified,
                         std::array<DecodedImageView, kMaxClassifiedTextures>& storage) noexcept;

[[nodiscard]] const char* j3d_material_family_result_name(J3dMaterialFamilyResult result) noexcept;
[[nodiscard]] const char* j3d_material_family_name(J3dMaterialFamily family) noexcept;

// `lighting` may be null, in which case every lit family is skipped and only the unlit ones can
// match. That is the honest answer before a stage's lighting has been published, not a fallback:
// a lit material classified against invented lighting would render wrongly rather than refuse.
[[nodiscard]] J3dMaterialFamilyResult classify_j3d_material(const J3dMaterialState& state,
                                                            const ModelLightingContext* lighting,
                                                            const J3dTextureSource& textures,
                                                            ClassifiedJ3dMaterial& out,
                                                            J3dFamilyRefusals* refusals) noexcept;

} // namespace sb::native_render
