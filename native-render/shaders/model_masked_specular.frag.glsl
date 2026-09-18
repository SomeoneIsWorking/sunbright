#version 450

layout(location = 0) in vec2 model_uv;
layout(location = 1) in vec4 model_color;
layout(location = 2) in vec4 model_additive_color;
layout(location = 3) in vec2 model_uv1;
layout(location = 0) out vec4 output_color;

layout(set = 2, binding = 0) uniform sampler2D model_mask_texture;
layout(set = 2, binding = 1) uniform sampler2D model_detail_texture;

#include "model_raster.glsl"

void main() {
    // The vertex boundary already carries the weighted lit colour plus the authored offset, so the
    // detail layer only adds its own image before clamping.
    vec3 detail_layer =
        clamp(texture(model_detail_texture, model_uv1).rgb + model_color.rgb, 0.0, 1.0);
    // The mask image chooses per channel between the highlight colour and that layer.
    vec4 mask = texture(model_mask_texture, model_uv);
    vec3 rgb = mix(model_additive_color.rgb, detail_layer, mask.rgb);
    // One of the two authored materials gates opacity with the mask image's alpha. The weight says
    // which, so both reach this program rather than one of them reaching a near-copy of it.
    float alpha = model_color.a * mix(1.0, mask.a, model_additive_color.a);
    output_color = apply_model_raster(vec4(clamp(rgb, 0.0, 1.0), alpha));
}
