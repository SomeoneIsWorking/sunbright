#version 450

layout(location = 0) in vec2 model_uv;
layout(location = 1) in vec4 model_color;
layout(location = 0) out vec4 output_color;

layout(set = 2, binding = 0) uniform sampler2D model_texture;

#include "model_raster.glsl"

void main() {
    // The authored second stage replaces alpha with a constant while the first stage preserves
    // texture RGB modulation. The semantic adapter resolves that constant before publication.
    vec4 sampled = texture(model_texture, model_uv);
    vec4 color = vec4(sampled.rgb * model_color.rgb, model_color.a);
    output_color = apply_model_raster(clamp(color, 0.0, 1.0));
}
