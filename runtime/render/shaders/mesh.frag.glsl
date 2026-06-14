#version 450
// Native J3D mesh pipeline (N4) — flat per-vertex color output. Materials/TEV are
// N5; this proves the geometry path (real game vertices -> native rasterized
// pixels) with the extracted vertex color0.
layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 o;
void main() { o = vColor; }
