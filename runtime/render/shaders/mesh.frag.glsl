#version 450
// Native J3D mesh pipeline (N5 first slice) — sample the bound GX texmap-0
// texture (decoded natively by the N1 decoder, per-batch) modulated by vertex
// color0. Batches with no/unsupported texture bind a 1×1 white texture, so the
// result reduces to the flat vertex color. Full TEV combiner is the next step.
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 0) out vec4 o;
layout(set = 0, binding = 0) uniform sampler2D tex;
void main() { o = texture(tex, vUV) * vColor; }
