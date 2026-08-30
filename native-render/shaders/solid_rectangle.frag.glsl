#version 450

layout(location = 0) in vec2 unused_uv;
layout(location = 1) in vec4 rectangle_color;
layout(location = 0) out vec4 output_color;

void main() {
    output_color = clamp(rectangle_color, 0.0, 1.0);
}
