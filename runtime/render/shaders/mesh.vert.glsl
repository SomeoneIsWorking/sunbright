#version 450
// Native J3D mesh pipeline (N4) — clip-space passthrough. The CPU side (the ngx
// J3DShape hook) has already done the full transform: model -> eye (modelview) ->
// clip (projection). We feed clip-space vec4 positions directly. Two adjustments
// to land in Vulkan's framebuffer space:
//   - Y flip: GX/GL NDC is +Y up, Vulkan is +Y down.
//   - depth remap: GC NDC z ∈ [-1(near), 0(far)] (Dolphin VertexShaderGen: near
//     "z < -w", far "z > 0") → Vulkan depth [0,1]. The faithful map is
//     depth = clip.z/w + 1, i.e. clip-space z' = clip.z + clip.w. near→0, far→1.
//     This also lets Vulkan's [0,w] depth clip do correct near/far frustum
//     clipping. (Exact GXSetViewport depth-range scaling — for depth-texture
//     effects — is a later refinement; occlusion sort order is faithful here.)
layout(location = 0) in vec4 clipPos;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 vColor;
void main() {
    vColor = color;
    gl_Position = vec4(clipPos.x, -clipPos.y, clipPos.z + clipPos.w, clipPos.w);
}
