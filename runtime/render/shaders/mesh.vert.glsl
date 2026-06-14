#version 450
// Native J3D mesh pipeline (N4) — clip-space passthrough. The CPU side (the ngx
// J3DShape hook) has already done the full transform: model -> eye (modelview) ->
// clip (projection). We feed clip-space vec4 positions directly. Two adjustments
// to land in Vulkan's framebuffer space:
//   - Y flip: GX/GL NDC is +Y up, Vulkan is +Y down.
//   - z forced to 0 (inside Vulkan's [0,w] depth clip): GX NDC z is not in [0,1],
//     so passing the real clip.z would let Vulkan depth-clip valid geometry. This
//     first-pixels milestone has no depth test (painter's/submission order); real
//     depth handling is a later refinement.
layout(location = 0) in vec4 clipPos;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 vColor;
void main() {
    vColor = color;
    gl_Position = vec4(clipPos.x, -clipPos.y, 0.0, clipPos.w);
}
