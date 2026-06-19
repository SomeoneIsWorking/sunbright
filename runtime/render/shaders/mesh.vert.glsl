#version 450
// Native J3D mesh pipeline (N4/N6) — clip-space passthrough. The CPU side (the ngx
// J3DShape hook) has already done the full transform: model -> eye (modelview) ->
// clip (projection), the per-vertex lighting (raster colour), and the per-texcoord
// GX texgen (uv[8], one per GX_TEXCOORD0..7). Two adjustments to land in Vulkan's
// framebuffer space:
//   - Y flip: GX/GL NDC is +Y up, Vulkan is +Y down.
//   - depth remap: GC NDC z in [-1(near),0(far)] -> Vulkan [0,1]: z' = z + w.
layout(location = 0) in vec4 clipPos;
layout(location = 1) in vec4 color;          // lit raster COLOR0
layout(location = 2) in vec2 uv[8];          // per-texcoord UV (GX texgen done on CPU)
layout(location = 10) in vec4 color1;        // lit raster COLOR1A1 (2nd GX colour channel)
layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV[8];
layout(location = 9) out vec4 vColor1;       // (locations 1..8 are vUV[8]; next free is 9)
void main() {
    vColor = color;
    vColor1 = color1;
    for (int i = 0; i < 8; i++) vUV[i] = uv[i];
    gl_Position = vec4(clipPos.x, -clipPos.y, clipPos.z + clipPos.w, clipPos.w);
}
