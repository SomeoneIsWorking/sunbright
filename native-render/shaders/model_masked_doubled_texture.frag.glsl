#version 450

layout(location = 0) in vec2 model_uv;
layout(location = 1) in vec4 model_color;
layout(location = 3) in vec2 model_uv1;
layout(location = 0) out vec4 output_color;

layout(set = 2, binding = 0) uniform sampler2D model_opacity_texture;
layout(set = 2, binding = 1) uniform sampler2D model_color_texture;

#include "model_raster.glsl"

void main() {
    // The first image's colour is not read: the stage that samples it computes one, and the stage
    // after it overwrites that colour rather than combining with it.
    float opacity = texture(model_opacity_texture, model_uv).a;
    vec4 layer = texture(model_color_texture, model_uv1);
    vec3 rgb = clamp(model_color.rgb * layer.rgb * 2.0, 0.0, 1.0);
    float alpha = clamp(clamp(model_color.a * opacity, 0.0, 1.0) * layer.a, 0.0, 1.0);
    output_color = apply_model_raster(vec4(rgb, alpha));
}
