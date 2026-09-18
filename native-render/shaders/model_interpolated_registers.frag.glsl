#version 450

layout(location = 0) in vec2 model_uv;
layout(location = 1) in vec4 model_color;
layout(location = 2) in vec4 model_additive_color;
layout(location = 3) in vec2 model_uv1;
layout(location = 4) in float model_detail_texture_weight;
layout(location = 8) in float model_texture_alpha_weight;
layout(location = 0) out vec4 output_color;

layout(set = 2, binding = 0) uniform sampler2D model_blend_texture;
layout(set = 2, binding = 1) uniform sampler2D model_detail_texture;

#include "model_raster.glsl"

void main() {
    vec4 blend = texture(model_blend_texture, model_uv);
    // The first image chooses per channel between the two authored registers, and the offset is
    // added to that choice before the stage clamps.
    vec3 chosen = mix(model_color.rgb, model_additive_color.rgb, blend.rgb);
    vec3 base = clamp(chosen + model_additive_color.a, 0.0, 1.0);
    vec3 detail = texture(model_detail_texture, model_uv1).rgb;
    // The second stage biases by a half and halves the sum: the authored program's own average,
    // not a tone curve applied on top of it.
    vec3 rgb = clamp((base + detail * model_detail_texture_weight + 0.5) * 0.5, 0.0, 1.0);
    float alpha = clamp(clamp(blend.a * model_color.a, 0.0, 1.0) * model_texture_alpha_weight, 0.0,
                        1.0);
    output_color = apply_model_raster(vec4(rgb, alpha));
}
