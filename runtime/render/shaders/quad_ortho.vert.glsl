#version 450
// Ortho textured-quad for the native J2D HUD renderer (N3). Per-quad pixel rect +
// target size come via push constants; the 4 strip vertices are generated from
// gl_VertexIndex. Pixel y=0 is the top (maps to Vulkan NDC -1), matching J2D and
// the texture's row-0-at-top convention.
layout(push_constant) uniform PC { vec4 rect; vec2 target; } pc;  // rect = x0,y0,x1,y1 (px)
layout(location = 0) out vec2 vUV;
void main() {
    vec2 c = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    vUV = c;
    vec2 px = mix(pc.rect.xy, pc.rect.zw, c);
    gl_Position = vec4(px / pc.target * 2.0 - 1.0, 0.0, 1.0);
}
