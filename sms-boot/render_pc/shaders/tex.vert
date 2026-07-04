#version 450
// Textured native vertex: NDC xyz + UV. The native engine supplies UVs it produced
// from the J3D texcoord arrays.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 0) out vec2 vUV;
void main() {
    gl_Position = vec4(inPos, 1.0);
    vUV = inUV;
}
