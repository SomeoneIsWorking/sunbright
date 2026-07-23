#version 450
// Texture RGB modulated by the vertex/material colour; ALPHA COMES FROM THE VERTEX ONLY.
//
// This is NOT the TEV pipeline. GX composes up to 16 stages, and crucially each stage's ALPHA
// combiner is configured separately from its colour combiner — a material commonly takes colour
// from the texture while taking alpha from a konst or the vertex.
//
// Multiplying the texture's alpha in is actively wrong for the intensity formats (I4/I8), where
// GX defines alpha = intensity: every dark texel then becomes transparent, and with the game's
// standard SRC_ALPHA/INV_SRC_ALPHA blend (measured on 920 of 929 drawables) the whole frame washes
// out toward the clear colour. That is exactly what it did.
//
// Taking alpha from the vertex is an explicit approximation with a known failure case (genuinely
// alpha-mapped textures render opaque) and is replaced when real TEV stages land.
layout(location = 0) in vec4 v_col;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D u_tex;
void main() {
    o_col = vec4(texture(u_tex, v_uv).rgb * v_col.rgb, v_col.a);
}
