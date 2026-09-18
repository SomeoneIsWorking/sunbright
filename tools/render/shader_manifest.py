"""Authoritative native-render shader inventory."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Shader:
    source: str
    stage: str
    symbol: str
    header: str


SHADERS = (
    Shader(
        "native-render/shaders/picture.vert.glsl",
        "vert",
        "kPictureVertSpv",
        "native-render/shaders/picture_vert_spv.h",
    ),
    Shader(
        "native-render/shaders/picture.frag.glsl",
        "frag",
        "kPictureFragSpv",
        "native-render/shaders/picture_frag_spv.h",
    ),
    Shader(
        "native-render/shaders/solid_rectangle.frag.glsl",
        "frag",
        "kSolidRectangleFragSpv",
        "native-render/shaders/solid_rectangle_frag_spv.h",
    ),
    Shader(
        "native-render/shaders/model.vert.glsl",
        "vert",
        "kModelVertSpv",
        "native-render/shaders/model_vert_spv.h",
    ),
    Shader(
        "native-render/shaders/model_color.frag.glsl",
        "frag",
        "kModelColorFragSpv",
        "native-render/shaders/model_color_frag_spv.h",
    ),
    Shader(
        "native-render/shaders/model_texture.frag.glsl",
        "frag",
        "kModelTextureFragSpv",
        "native-render/shaders/model_texture_frag_spv.h",
    ),
    Shader(
        "native-render/shaders/model_texture_constant_alpha.frag.glsl",
        "frag",
        "kModelTextureConstantAlphaFragSpv",
        "native-render/shaders/model_texture_constant_alpha_frag_spv.h",
    ),
    Shader(
        "native-render/shaders/model_dual_alpha_effect.frag.glsl",
        "frag",
        "kModelDualAlphaEffectFragSpv",
        "native-render/shaders/model_dual_alpha_effect_frag_spv.h",
    ),
    Shader(
        "native-render/shaders/model_lit_alpha_mask.frag.glsl",
        "frag",
        "kModelLitAlphaMaskFragSpv",
        "native-render/shaders/model_lit_alpha_mask_frag_spv.h",
    ),
    Shader(
        "native-render/shaders/model_lit_alpha_tint.frag.glsl",
        "frag",
        "kModelLitAlphaTintFragSpv",
        "native-render/shaders/model_lit_alpha_tint_frag_spv.h",
    ),
    Shader(
        "native-render/shaders/model_doubled_texture_pair.frag.glsl",
        "frag",
        "kModelDoubledTexturePairFragSpv",
        "native-render/shaders/model_doubled_texture_pair_frag_spv.h",
    ),
    Shader(
        "native-render/shaders/model_tinted_texture_sum.frag.glsl",
        "frag",
        "kModelTintedTextureSumFragSpv",
        "native-render/shaders/model_tinted_texture_sum_frag_spv.h",
    ),
    Shader(
        "native-render/shaders/model_masked_doubled_texture.frag.glsl",
        "frag",
        "kModelMaskedDoubledTextureFragSpv",
        "native-render/shaders/model_masked_doubled_texture_frag_spv.h",
    ),
    Shader(
        "native-render/shaders/model_interpolated_registers.frag.glsl",
        "frag",
        "kModelInterpolatedRegistersFragSpv",
        "native-render/shaders/model_interpolated_registers_frag_spv.h",
    ),
    Shader(
        "native-render/shaders/model_layered_lit.frag.glsl",
        "frag",
        "kModelLayeredLitFragSpv",
        "native-render/shaders/model_layered_lit_frag_spv.h",
    ),
    Shader(
        "native-render/shaders/model_tinted_layered.frag.glsl",
        "frag",
        "kModelTintedLayeredFragSpv",
        "native-render/shaders/model_tinted_layered_frag_spv.h",
    ),
    Shader(
        "native-render/shaders/model_masked_specular.frag.glsl",
        "frag",
        "kModelMaskedSpecularFragSpv",
        "native-render/shaders/model_masked_specular_frag_spv.h",
    ),
    Shader(
        "native-render/shaders/model_masked_toon.frag.glsl",
        "frag",
        "kModelMaskedToonFragSpv",
        "native-render/shaders/model_masked_toon_frag_spv.h",
    ),
)
