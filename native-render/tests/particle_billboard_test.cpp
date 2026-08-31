#include <sunbright/native_render/particle_billboard.h>

#include <cassert>

int main() {
    const sb::native_render::ParticleBillboardInput input{
        .eyeCenter = {10.0F, 20.0F, 30.0F},
        .halfExtent = {2.0F, 3.0F},
        .pivot = {0.25F, -0.5F},
        .uv = {{{0.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 1.0F}, {0.0F, 1.0F}}},
        .vertexColor = {0.5F, 0.25F, 0.75F, 1.0F},
    };
    const auto mesh = sb::native_render::make_particle_billboard_mesh(input);
    const sb::native_render::Vec3 topLeft{7.5F, 21.5F, 30.0F};
    const sb::native_render::Vec3 topRight{11.5F, 21.5F, 30.0F};
    const sb::native_render::Vec3 bottomRight{11.5F, 15.5F, 30.0F};
    assert(mesh[0].position == topLeft);
    assert(mesh[1].position == topRight);
    assert(mesh[2].position == bottomRight);
    assert(mesh[3].position == mesh[0].position);
    assert(mesh[4].uv == input.uv[2]);
    assert(mesh[5].color == input.vertexColor);
}
