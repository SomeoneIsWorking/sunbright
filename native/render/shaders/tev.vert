#version 450
// TEV vertex shader: pass the engine-computed clip/NDC position, both GX raster
// colour channels, and the 8 texgen'd UVs through to the generated TEV fragment
// shader (sb_tev_gen_fragment). The UVs arrive packed two-per-vec4 attribute.
layout(location = 0) in vec4 inPos;   // CLIP-space xyzw (the GPU divides → perspective-correct)
layout(location = 1) in vec4 inColor0;
layout(location = 2) in vec4 inColor1;
layout(location = 3) in vec4 inUV01;   // (uv0.xy, uv1.xy)
layout(location = 4) in vec4 inUV23;
layout(location = 5) in vec4 inUV45;
layout(location = 6) in vec4 inUV67;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV[8];
layout(location = 9) out vec4 vColor1;

void main() {
    gl_Position = inPos;
    vColor  = inColor0;
    vColor1 = inColor1;
    vUV[0] = inUV01.xy; vUV[1] = inUV01.zw;
    vUV[2] = inUV23.xy; vUV[3] = inUV23.zw;
    vUV[4] = inUV45.xy; vUV[5] = inUV45.zw;
    vUV[6] = inUV67.xy; vUV[7] = inUV67.zw;
}
