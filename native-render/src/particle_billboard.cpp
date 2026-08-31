#include <sunbright/native_render/particle_billboard.h>

namespace sb::native_render {

std::array<MeshVertex, 6>
make_particle_billboard_mesh(const ParticleBillboardInput& input) noexcept {
    const float left = -input.halfExtent.x * (1.0F + input.pivot.x);
    const float right = input.halfExtent.x * (1.0F - input.pivot.x);
    const float top = input.halfExtent.y * (1.0F + input.pivot.y);
    const float bottom = -input.halfExtent.y * (1.0F - input.pivot.y);
    const std::array<Vec3, 4> positions{{
        {input.eyeCenter.x + left, input.eyeCenter.y + top, input.eyeCenter.z},
        {input.eyeCenter.x + right, input.eyeCenter.y + top, input.eyeCenter.z},
        {input.eyeCenter.x + right, input.eyeCenter.y + bottom, input.eyeCenter.z},
        {input.eyeCenter.x + left, input.eyeCenter.y + bottom, input.eyeCenter.z},
    }};
    const auto vertex = [&](std::size_t index) {
        return MeshVertex{
            .position = positions[index], .uv = input.uv[index], .color = input.vertexColor};
    };
    return {vertex(0), vertex(1), vertex(2), vertex(0), vertex(2), vertex(3)};
}

} // namespace sb::native_render
