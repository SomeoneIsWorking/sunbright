#version 450
// Texture × interpolated corner color (J2DPicture RASC modulation).
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vCol;
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uTex;
void main() { oColor = texture(uTex, vUV) * vCol; }
