#version 450
// Milestone 1 of the native GX renderer: positions arrive already in CLIP space (the frontend
// applies posMtx then the projection on the CPU, matching the retired Path-B NvkTevVertex
// contract), so the vertex stage is a pass-through. Colour is per-vertex so a draw can be tinted
// for identification without a pipeline change.
layout(location = 0) in vec4 in_pos;    // clip space
layout(location = 1) in vec4 in_col;
layout(location = 0) out vec4 v_col;
void main() {
    gl_Position = in_pos;
    v_col = in_col;
}
