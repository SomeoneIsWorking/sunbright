#include <sunbright/native_render/model.h>

#include <cassert>
#include <cmath>
#include <limits>

namespace {

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001F;
}

} // namespace

int main() {
    using namespace sb::native_render;

    const MeshVertex vertex{{1.0F, 2.0F, 3.0F}, {0.25F, 0.5F}, {0.5F, 0.25F, 1.0F, 0.5F}};
    const ModelDraw draw{
        .instance = 7,
        .mesh = {.resource = 11, .revision = 2, .vertexCount = 3},
        .modelView = {.value = {1, 0, 0, 4, 0, 1, 0, 5, 0, 0, 1, 6}},
        .projection = {.value = {2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1}},
        .material =
            UnlitColorMaterial{.baseColor = {0.5F, 1.0F, 0.25F, 0.5F}, .usesVertexColor = true},
    };

    assert(valid(vertex));
    assert(valid(draw));
    const ClipVertex transformed = transform_vertex(draw, vertex);
    assert(near(transformed.position.x, 10.0F));
    assert(near(transformed.position.y, 21.0F));
    assert(near(transformed.position.z, 36.0F));
    assert(near(transformed.position.w, 1.0F));
    assert(near(transformed.color.r, 0.25F));
    assert(near(transformed.color.g, 0.25F));
    assert(near(transformed.color.b, 0.25F));
    assert(near(transformed.color.a, 0.25F));

    const Matrix4x4 j3dProjection{.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, -2, 0, 0, -1, 0}};
    const Matrix4x4 depthConverted = zero_to_one_depth_projection(j3dProjection);
    assert(depthConverted.value[10] == -1);
    assert(depthConverted.value[11] == -2);

    const MeshVertex revisions[2]{vertex, vertex};
    assert(mesh_revision(revisions) == mesh_revision(revisions));
    MeshVertex changed[2]{vertex, vertex};
    changed[1].position.x += 1.0F;
    assert(mesh_revision(revisions) != mesh_revision(changed));

    ModelDraw constantColor = draw;
    auto& constantMaterial = std::get<UnlitColorMaterial>(constantColor.material);
    constantMaterial.usesVertexColor = false;
    const ClipVertex constant = transform_vertex(constantColor, vertex);
    assert(constant.color == constantMaterial.baseColor);

    MeshResourceView mesh{11, 2, std::span(&vertex, 1)};
    assert(!valid(mesh));
    MeshVertex triangle[3]{vertex, vertex, vertex};
    mesh.vertices = triangle;
    assert(valid(mesh));

    ModelDraw invalid = draw;
    invalid.mesh.vertexCount = 4;
    assert(!valid(invalid));
    invalid = draw;
    invalid.projection.value[0] = std::numeric_limits<float>::infinity();
    assert(!valid(invalid));
}
