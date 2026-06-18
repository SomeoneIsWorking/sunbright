#version 450
// Pollution shine-shadow-volume sphere. CPU pushes mvp = g_proj(4x4) * model(unit->eye),
// where model = translate(eyeSphereCenter) * scale(radius). Same clip-space landing as
// mesh.vert (Y flip + GC depth remap z'=z+w), so the sphere z-tests against the scene depth.
layout(location = 0) in vec3 inPos;
layout(location = 0) out vec2 vUV;
layout(push_constant) uniform P { mat4 mvp; vec4 color; } p;
void main() {
    vUV = vec2(0.0);
    vec4 clip = p.mvp * vec4(inPos, 1.0);
    gl_Position = vec4(clip.x, -clip.y, clip.z + clip.w, clip.w);
}
