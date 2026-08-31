#version 450

layout(location = 0) in vec2 model_uv;
layout(location = 1) in vec4 model_color;
layout(location = 2) in vec4 model_additive_color;
layout(location = 3) in vec2 model_uv1;
layout(location = 4) in float model_light_ramp_weight;
layout(location = 6) in vec2 model_uv2;
layout(location = 7) in vec2 model_uv3;
layout(location = 0) out vec4 output_color;

layout(set = 2, binding = 0) uniform sampler2D model_primary_texture;
layout(set = 2, binding = 1) uniform sampler2D model_mask_texture;
layout(set = 2, binding = 2) uniform sampler2D model_alternate_texture;
layout(set = 2, binding = 3) uniform sampler2D model_light_ramp_texture;

#include "model_raster.glsl"

void main() {
    // The game authored the mask at 8-bit precision. Quantising this semantic mask sample keeps
    // the intended above/below-128 choice without recreating the console's colour-stage machine.
    float mask_alpha = texture(model_mask_texture, model_uv1).a;
    bool select_primary = floor(clamp(mask_alpha, 0.0, 1.0) * 255.0 + 0.5) > 128.0;
    vec3 base = select_primary ? texture(model_primary_texture, model_uv).rgb
                               : texture(model_alternate_texture, model_uv2).rgb;
    vec3 light_ramp = texture(model_light_ramp_texture, model_uv3).rgb;
    vec3 light_layer = clamp(light_ramp * model_light_ramp_weight +
                                 model_color.rgb * (1.0 - model_light_ramp_weight) - 0.5,
                             0.0, 1.0);
    vec4 color = vec4(clamp(base + light_layer + model_additive_color.rgb, 0.0, 1.0),
                      model_additive_color.a);
    output_color = apply_model_raster(color);
}
