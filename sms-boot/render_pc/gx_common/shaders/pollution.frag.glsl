#version 450
// Delfino plaza pollution darkening (TModelWaterManager::drawShineShadowVolume port).
// Outputs a flat push-constant colour. Used by all three pollution passes:
//   - alpha-clear  (fullscreen, colorWriteMask=A): writes EFB alpha := clearAlpha
//   - sphere volume (colorWriteMask=A): writes the volume alpha (saturates inside)
//   - final blend  (fullscreen, colorWriteMask=RGB, INVDSTALPHA/DSTALPHA): dark-blue tint
layout(location = 0) in vec2 vUV;   // unused (matches quad.vert / pollution_sphere.vert output)
layout(location = 0) out vec4 o;
layout(push_constant) uniform P { mat4 mvp; vec4 color; } p;
void main() { o = p.color; }
