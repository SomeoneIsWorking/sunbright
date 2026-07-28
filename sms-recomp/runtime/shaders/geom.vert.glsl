#version 450
// Positions arrive already in CLIP space (the frontend applies the draw matrix then the projection
// on the CPU, matching the retired Path-B NvkTevVertex contract), so the vertex stage is a
// pass-through. Colour is the lit colour channel; the coordinates are already texgen'd — the
// frontend evaluates the source select and the texture matrix per vertex, because the sources
// include the position and normal and the matrices animate per frame.
layout(location = 0) in vec4 in_pos;    // clip space
layout(location = 1) in vec4 in_col;
layout(location = 2) in vec4 in_uv01;   // texgen 0 in .xy, texgen 1 in .zw
layout(location = 3) in vec4 in_uv23;
layout(location = 4) in vec4 in_col1;   // rasterised colour channel 1, carried but not yet selected
layout(location = 0) out vec4 v_col;
layout(location = 1) out vec4 v_uv01;
layout(location = 2) out vec4 v_uv23;
layout(location = 3) out vec4 v_col1;
void main() {
    gl_Position = in_pos;
    v_col = in_col;
    v_uv01 = in_uv01;
    v_uv23 = in_uv23;
    v_col1 = in_col1;
}
