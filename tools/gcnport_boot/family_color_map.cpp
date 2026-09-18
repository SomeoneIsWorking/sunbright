// SPDX-License-Identifier: GPL-2.0-or-later
#include "family_color_map.h"

namespace sunbright::gcnport_boot {

using sb::native_render::Color;
using sb::native_render::J3dMaterialFamily;

Color family_map_color(J3dMaterialFamily family) noexcept {
    // Chosen to be told apart in a small image rather than to be pretty. Deliberately no pure
    // white and no fully saturated channel: the defect this map exists to attribute makes surfaces
    // come out white, and a legend entry a blown-out draw could imitate would be read as the
    // family rather than as the defect.
    switch (family) {
    case J3dMaterialFamily::None:
        return {0.10F, 0.10F, 0.10F, 1.0F};
    case J3dMaterialFamily::UnlitColor:
        return {0.85F, 0.10F, 0.10F, 1.0F};
    case J3dMaterialFamily::LitColor:
        return {0.10F, 0.70F, 0.10F, 1.0F};
    case J3dMaterialFamily::LitSpecularRamp:
        return {0.10F, 0.15F, 0.85F, 1.0F};
    case J3dMaterialFamily::LitSpecularColor:
        return {0.80F, 0.75F, 0.10F, 1.0F};
    case J3dMaterialFamily::UnlitTextured:
        return {0.85F, 0.10F, 0.75F, 1.0F};
    case J3dMaterialFamily::AlphaMaskedColor:
        return {0.10F, 0.75F, 0.80F, 1.0F};
    case J3dMaterialFamily::LitSpecularTextured:
        return {0.85F, 0.45F, 0.10F, 1.0F};
    case J3dMaterialFamily::LitTextured:
        return {0.45F, 0.10F, 0.85F, 1.0F};
    case J3dMaterialFamily::LitTexturedAlphaMask:
        return {0.10F, 0.45F, 0.85F, 1.0F};
    case J3dMaterialFamily::LitDualAlphaEffect:
        return {0.85F, 0.10F, 0.40F, 1.0F};
    case J3dMaterialFamily::LitAlphaTint:
        return {0.55F, 0.80F, 0.10F, 1.0F};
    case J3dMaterialFamily::LitLayeredTextured:
        return {0.10F, 0.80F, 0.50F, 1.0F};
    case J3dMaterialFamily::LitTintedLayeredSpecular:
        return {0.40F, 0.05F, 0.05F, 1.0F};
    case J3dMaterialFamily::LitMaskedToon:
        return {0.05F, 0.35F, 0.05F, 1.0F};
    case J3dMaterialFamily::LitMaskedSpecular:
        return {0.05F, 0.05F, 0.40F, 1.0F};
    case J3dMaterialFamily::TexturedEffect:
        return {0.40F, 0.35F, 0.05F, 1.0F};
    case J3dMaterialFamily::UnlitTexturedEffect:
        return {0.40F, 0.05F, 0.35F, 1.0F};
    case J3dMaterialFamily::DoubledTexturePair:
        return {0.05F, 0.35F, 0.40F, 1.0F};
    case J3dMaterialFamily::TintedTextureSum:
        return {0.75F, 0.60F, 0.45F, 1.0F};
    case J3dMaterialFamily::MaskedDoubledTexture:
        return {0.55F, 0.55F, 0.55F, 1.0F};
    case J3dMaterialFamily::InterpolatedRegisters:
        return {0.25F, 0.25F, 0.25F, 1.0F};
    }
    return {0.0F, 0.0F, 0.0F, 1.0F};
}

sb::native_render::ModelMaterial
family_map_material(J3dMaterialFamily family,
                    const sb::native_render::ModelRasterPolicy& raster) noexcept {
    sb::native_render::ModelRasterPolicy opaque = raster;
    opaque.blend = sb::native_render::ModelBlendMode::Replace;
    opaque.alphaTest = sb::native_render::ModelAlphaTest::PassAll;
    return sb::native_render::UnlitColorMaterial{
        .baseColor = family_map_color(family), .usesVertexColor = false, .raster = opaque};
}

sb::native_render::ModelMaterial
opaque_material(sb::native_render::ModelMaterial material) noexcept {
    sb::native_render::ModelRasterPolicy& raster = sb::native_render::raster_policy(material);
    raster.blend = sb::native_render::ModelBlendMode::Replace;
    raster.alphaTest = sb::native_render::ModelAlphaTest::PassAll;
    return material;
}

} // namespace sunbright::gcnport_boot
