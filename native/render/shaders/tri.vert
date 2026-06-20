#version 450
// Minimal native-renderer vertex shader: NDC position + per-vertex RGBA color.
// The native engine feeds geometry it produced itself (e.g. GXProject screen coords
// converted to NDC) — no GameCube vertex pipeline.
layout(location = 0) in vec2 inPos;   // clip/NDC xy
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 vColor;
void main() {
    gl_Position = vec4(inPos, 0.0, 1.0);
    vColor = inColor;
}
