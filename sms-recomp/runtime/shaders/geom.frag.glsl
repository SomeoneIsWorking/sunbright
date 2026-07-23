#version 450
// Texture x vertex colour. This is NOT the TEV pipeline: GX composes up to 16 stages with
// configurable inputs, and this is the single most common case (one texture modulated by the
// vertex/material colour). Real TEV stages are the next milestone; until then this is deliberately
// an approximation, not a claim of parity.
layout(location = 0) in vec4 v_col;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D u_tex;
void main() {
    o_col = texture(u_tex, v_uv) * v_col;
}
