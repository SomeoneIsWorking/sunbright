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
    float raster_alpha = model_color.a;
    float alpha = raster_alpha *
                  (2.0 * (texture(model_first_texture, model_uv).a *
                          texture(model_second_texture, model_uv1).a +
                          raster_alpha));
    vec4 color = clamp(vec4(model_color.rgb + model_additive_color.rgb, alpha), 0.0, 1.0);
    output_color = apply_model_raster(color);
}
