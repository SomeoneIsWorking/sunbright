#version 450

layout(location = 0) in vec2 model_uv;
layout(location = 1) in vec4 model_color;
layout(location = 2) in vec4 model_additive_color;
layout(location = 0) out vec4 output_color;

layout(set = 2, binding = 0) uniform sampler2D model_texture;
layout(set = 3, binding = 0) uniform ModelRasterBlock {
    vec4 alpha_test;
};

void main() {
    vec4 color = clamp(texture(model_texture, model_uv) * model_color + model_additive_color,
                       0.0, 1.0);
    if (color.a < alpha_test.x) {
        discard;
    }
    output_color = color;
}
