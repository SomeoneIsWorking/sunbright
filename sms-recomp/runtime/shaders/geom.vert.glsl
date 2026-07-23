#version 450
// Positions arrive already in CLIP space (the frontend applies the draw matrix then the projection
// on the CPU, matching the retired Path-B NvkTevVertex contract), so the vertex stage is a
// pass-through. Colour is the material/vertex colour; UV indexes TEXMAP0.
layout(location = 0) in vec4 in_pos;    // clip space
layout(location = 1) in vec4 in_col;
layout(location = 2) in vec2 in_uv;
layout(location = 0) out vec4 v_col;
layout(location = 1) out vec2 v_uv;
void main() {
    gl_Position = in_pos;
    v_col = in_col;
    v_uv = in_uv;
}
