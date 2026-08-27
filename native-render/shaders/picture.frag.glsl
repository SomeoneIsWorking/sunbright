#version 450

layout(location = 0) in vec2 picture_uv;
layout(location = 1) in vec4 picture_color;
layout(location = 0) out vec4 output_color;

layout(set = 2, binding = 0) uniform sampler2D picture_texture_0;
layout(set = 2, binding = 1) uniform sampler2D picture_texture_1;
layout(set = 2, binding = 2) uniform sampler2D picture_texture_2;
layout(set = 2, binding = 3) uniform sampler2D picture_texture_3;

layout(set = 3, binding = 0) uniform PictureStyleBlock {
    vec4 black;
    vec4 white;
    vec4 color_mix;
    vec4 alpha_mix;
    ivec4 control;
    vec4 opacity;
} style;

vec4 sample_layer(int layer) {
    if (layer == 1)
        return texture(picture_texture_1, picture_uv);
    if (layer == 2)
        return texture(picture_texture_2, picture_uv);
    if (layer == 3)
        return texture(picture_texture_3, picture_uv);
    return texture(picture_texture_0, picture_uv);
}

void main() {
    vec4 result = sample_layer(0);
    if ((style.control.y & 1) == 0)
        result.a = 1.0;
    for (int layer = 1; layer < style.control.x; ++layer) {
        vec4 sample_value = sample_layer(layer);
        if ((style.control.y & (1 << layer)) == 0)
            sample_value.a = 1.0;
        result.rgb = mix(result.rgb, sample_value.rgb, style.color_mix[layer]);
        result.a = mix(result.a, sample_value.a, style.alpha_mix[layer]);
    }
    result = mix(style.black, style.white, result);
    result *= picture_color;
    result.a *= style.opacity.x;
    output_color = clamp(result, 0.0, 1.0);
}
