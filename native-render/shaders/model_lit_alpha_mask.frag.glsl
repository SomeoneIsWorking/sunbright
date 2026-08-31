#version 450

layout(location = 0) in vec2 model_uv;
layout(location = 1) in vec4 model_color;
layout(location = 3) in vec2 model_uv1;
layout(location = 0) out vec4 output_color;

layout(set = 2, binding = 0) uniform sampler2D model_color_texture;
layout(set = 2, binding = 1) uniform sampler2D model_alpha_mask_texture;
layout(set = 3, binding = 0) uniform ModelRasterBlock {
    vec4 alpha_test;
};

void main() {
    vec4 color = vec4(texture(model_color_texture, model_uv).rgb * model_color.rgb,
                      texture(model_alpha_mask_texture, model_uv1).a * model_color.a);
    color = clamp(color, 0.0, 1.0);
    if (color.a < alpha_test.x) {
        discard;
    }
    output_color = color;
}
