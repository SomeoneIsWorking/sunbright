#version 450

layout(location = 0) in vec2 model_uv;
layout(location = 1) in vec4 model_color;
layout(location = 2) in vec4 model_additive_color;
layout(location = 3) in vec2 model_uv1;
layout(location = 0) out vec4 output_color;

layout(set = 2, binding = 0) uniform sampler2D model_base_texture;
layout(set = 2, binding = 1) uniform sampler2D model_detail_texture;

#include "model_raster.glsl"

void main() {
    vec4 base = texture(model_base_texture, model_uv);
    vec4 detail = texture(model_detail_texture, model_uv1);
    // Both authored spellings multiply the pair by one tint and double the result. The vertex
    // boundary has already resolved where that tint came from.
    vec3 rgb = clamp(model_color.rgb * base.rgb * detail.rgb * 2.0, 0.0, 1.0);
    // One spelling carries the textures' own alpha through, doubled alongside the colour; the
    // other leaves opacity to authored constants alone. The weight says which, so both reach this
    // program rather than one of them reaching a near-copy of it.
    float textured_alpha = clamp(model_color.a * base.a * detail.a * 2.0, 0.0, 1.0);
    float alpha = mix(model_color.a, textured_alpha, model_additive_color.a);
    output_color = apply_model_raster(vec4(rgb, alpha));
}
