#version 450

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 picture_uv;
layout(location = 1) out vec4 picture_color;

layout(set = 1, binding = 0) uniform CanvasBlock {
    vec2 origin;
    vec2 extent;
} canvas;

void main() {
    vec2 normalized = (in_position - canvas.origin) / canvas.extent;
    gl_Position = vec4(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0, 0.0, 1.0);
    picture_uv = in_uv;
    picture_color = in_color;
}
