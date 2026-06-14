#version 450
// Ortho textured-quad for the native J2D renderer (N3) with per-corner color +
// pane-alpha modulation (J2DPicture's RASC × mColorAlpha path). Per-quad pixel
// rect, target size, packed RGBA8 corner colors, and colorAlpha come via push
// constants; the 4 strip vertices + their corner color are picked from
// gl_VertexIndex. Pixel y=0 = top (Vulkan NDC -1), matching J2D + texture rows.
layout(push_constant) uniform PC {
    vec4  rect;       // x0,y0,x1,y1 (px)
    vec4  misc;       // misc.xy = target(w,h); misc.z = colorAlpha (0..1)
    uvec4 corners;    // packed 0xRRGGBBAA per corner: [TL,TR,BL,BR]
} pc;
layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vCol;
void main() {
    uint ix = uint(gl_VertexIndex);
    vec2 c = vec2(float(ix & 1u), float((ix >> 1) & 1u));   // TL,TR,BL,BR
    uint k = pc.corners[(ix & 1u) + ((ix >> 1) & 1u) * 2u];
    vec4 col = vec4(float((k >> 24) & 0xffu), float((k >> 16) & 0xffu),
                    float((k >> 8) & 0xffu),  float(k & 0xffu)) / 255.0;
    col.a *= pc.misc.z;
    vUV = c;
    vCol = col;
    gl_Position = vec4(mix(pc.rect.xy, pc.rect.zw, c) / pc.misc.xy * 2.0 - 1.0, 0.0, 1.0);
}
