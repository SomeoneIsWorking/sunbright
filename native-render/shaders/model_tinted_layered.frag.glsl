#version 450

layout(location = 0) in vec2 model_uv;
layout(location = 1) in vec4 model_color;
layout(location = 2) in vec4 model_additive_color;
layout(location = 3) in vec2 model_uv1;
layout(location = 4) in float model_detail_texture_weight;
layout(location = 0) out vec4 output_color;

layout(set = 2, binding = 0) uniform sampler2D model_base_texture;
layout(set = 2, binding = 1) uniform sampler2D model_detail_texture;

#include "model_raster.glsl"

void main() {
    vec3 detail_layer = clamp(model_color.rgb +
                                  texture(model_detail_texture, model_uv1).rgb *
                                      model_detail_texture_weight,
                              0.0, 1.0);
    vec4 color = vec4(clamp(texture(model_base_texture, model_uv).rgb +
                                detail_layer * model_additive_color.a +
                                model_additive_color.rgb - 0.5,
                            0.0, 1.0),
                      model_color.a);
    output_color = apply_model_raster(color);
}
