#version 450
// Fullscreen-triangle textured quad (N2 bring-up). No vertex buffer: the 3
// vertices are generated from gl_VertexIndex. UV 0..1 covers the target; with a
// nearest sampler and a target sized to the texture, output texel (x,y) samples
// source texel (x,y) exactly (used for the offscreen parity self-test).
layout(location = 0) out vec2 vUV;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vUV = p;                                   // 0,0 / 2,0 / 0,2
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
