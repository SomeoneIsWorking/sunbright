#version 450

layout(location = 0) in vec2 model_uv;
layout(location = 1) in vec4 model_color;
layout(location = 0) out vec4 output_color;

layout(set = 2, binding = 0) uniform sampler2D model_texture;

#include "model_raster.glsl"

void main() {
    // The authored stage uses the texture only for alpha. Its RGB is diffuse lighting multiplied
    // by the resolved register tint, already published in model_color by the semantic adapter.
    vec4 color = vec4(model_color.rgb, texture(model_texture, model_uv).a * model_color.a);
    output_color = apply_model_raster(clamp(color, 0.0, 1.0));
}
