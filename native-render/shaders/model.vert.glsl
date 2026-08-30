#version 450

layout(location = 0) in vec4 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 model_uv;
layout(location = 1) out vec4 model_color;

void main() {
    gl_Position = in_position;
    model_uv = in_uv;
    model_color = in_color;
}
