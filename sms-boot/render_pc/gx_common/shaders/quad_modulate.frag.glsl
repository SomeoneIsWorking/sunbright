#version 450
// J2DPicture::setTevMode for one texture: optional intensity black/white remap
// (TEVREG C0/C1 lerp'd by the texel — for I/IA-format glyphs/trails), then ×
// interpolated corner color (RASC modulation). For a normal RGBA picture the game
// leaves black=0/white=1 so the lerp is identity → plain texture × corner color.
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vCol;
layout(location = 2) flat in vec4 vWhite;
layout(location = 3) flat in vec4 vBlack;
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uTex;
void main() {
    vec4 t = texture(uTex, vUV);
    vec4 c = mix(vBlack, vWhite, t);   // per-channel C0*(1-t)+C1*t (GX black/white stage)
    oColor = c * vCol;
}
