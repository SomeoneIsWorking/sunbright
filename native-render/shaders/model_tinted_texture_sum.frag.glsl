#version 450

layout(location = 0) in vec2 model_uv;
layout(location = 1) in vec4 model_color;
layout(location = 2) in vec4 model_additive_color;
layout(location = 3) in vec2 model_uv1;
layout(location = 0) out vec4 output_color;

layout(set = 2, binding = 0) uniform sampler2D model_first_texture;
layout(set = 2, binding = 1) uniform sampler2D model_second_texture;

#include "model_raster.glsl"

void main() {
    vec4 first = texture(model_first_texture, model_uv);
    vec4 second = texture(model_second_texture, model_uv1);
    // Each layer is clamped where the stage that produced it clamps, not only at the end: the
    // first layer saturates before the second is added to it.
    vec3 first_layer = clamp(model_color.rgb * first.rgb, 0.0, 1.0);
    vec3 rgb = clamp(first_layer + model_additive_color.rgb * second.rgb, 0.0, 1.0);
    float alpha = clamp(clamp(model_color.a * first.a, 0.0, 1.0) * second.a, 0.0, 1.0);
    output_color = apply_model_raster(vec4(rgb, alpha));
}
