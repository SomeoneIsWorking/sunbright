#version 450

layout(location = 0) in vec4 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 3) in vec4 in_additive_color;
layout(location = 4) in vec2 in_uv1;
layout(location = 5) in float in_detail_texture_weight;

layout(location = 0) out vec2 model_uv;
layout(location = 1) out vec4 model_color;
layout(location = 2) out vec4 model_additive_color;
layout(location = 3) out vec2 model_uv1;
layout(location = 4) out float model_detail_texture_weight;

void main() {
    gl_Position = in_position;
    model_uv = in_uv;
    model_color = in_color;
    model_additive_color = in_additive_color;
    model_uv1 = in_uv1;
    model_detail_texture_weight = in_detail_texture_weight;
}
