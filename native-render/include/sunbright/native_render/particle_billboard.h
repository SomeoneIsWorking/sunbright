#pragma once

#include <sunbright/native_render/model.h>

#include <array>

namespace sb::native_render {

// A JPA billboard has a fixed quad shape in eye space; only its centre comes from the particle.
// Keeping this construction in the shared renderer layer makes the decomp and recomp adapters
// publish the same six-vertex triangle list without exposing GX vertex emission to either one.
struct ParticleBillboardInput {
    Vec3 eyeCenter{};
    Vec2 halfExtent{};
    Vec2 pivot{};
    std::array<Vec2, 4> uv{};
    Color vertexColor{1.0F, 1.0F, 1.0F, 1.0F};
};

[[nodiscard]] std::array<MeshVertex, 6>
make_particle_billboard_mesh(const ParticleBillboardInput& input) noexcept;

} // namespace sb::native_render
