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

    const MeshVertex vertex{.position = {1.0F, 2.0F, 3.0F},
                            .uv = {0.25F, 0.5F},
                            .uv1 = {0.75F, 0.125F},
                            .color = {0.5F, 0.25F, 1.0F, 0.5F}};
    const ModelDraw draw{
        .instance = 7,
        .mesh = {.resource = 11, .revision = 2, .vertexCount = 3},
        .pose = {.modelViews = {Matrix3x4{.value = {1, 0, 0, 4, 0, 1, 0, 5, 0, 0, 1, 6}}},
                 .count = 1},
        .projection = {.value = {2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1}},
        .material =
            UnlitColorMaterial{.baseColor = {0.5F, 1.0F, 0.25F, 0.5F}, .usesVertexColor = true},
    };

    assert(valid(vertex));
    assert(valid(draw));
    assert(raster_policy(draw.material) == ModelRasterPolicy{});
    assert(material_texture_count(draw.material) == 0);
    assert(material_texture(draw.material) == nullptr);
    const ClipVertex transformed = transform_vertex(draw, vertex);
    assert(near(transformed.position.x, 10.0F));
    assert(near(transformed.position.y, 21.0F));
    assert(near(transformed.position.z, 36.0F));
    assert(near(transformed.position.w, 1.0F));
    assert(near(transformed.eyeDepth, -9.0F));
    assert(transformed.uv1 == vertex.uv1);
    assert(near(transformed.color.r, 0.25F));
    assert(near(transformed.color.g, 0.25F));
    assert(near(transformed.color.b, 0.25F));
    assert(near(transformed.color.a, 0.25F));
    assert(transformed.additiveColor == Color{});

    ModelDraw fogged = draw;
    fogged.fog = {.mode = ModelFogMode::Linear, .start = 8.0F, .end = 10.0F, .color = {1, 0, 0, 1}};
    assert(valid(fogged));
    fogged.fog.end = fogged.fog.start;
    assert(!valid(fogged));

    const Matrix4x4 j3dProjection{.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, -2, 0, 0, -1, 0}};
    const Matrix4x4 depthConverted = zero_to_one_depth_projection(j3dProjection);
    assert(depthConverted.value[10] == -1);
    assert(depthConverted.value[11] == -2);

    const MeshVertex revisions[2]{vertex, vertex};
    assert(mesh_revision(revisions) == mesh_revision(revisions));
    MeshVertex changed[2]{vertex, vertex};
    changed[1].position.x += 1.0F;
    assert(mesh_revision(revisions) != mesh_revision(changed));
    changed[1] = vertex;
    changed[1].normal.x += 1.0F;
    assert(mesh_revision(revisions) != mesh_revision(changed));
    changed[1] = vertex;
    changed[1].matrixIndex = 1;
    assert(mesh_revision(revisions) != mesh_revision(changed));

    ModelDraw posed = draw;
    posed.pose.modelViews[1] = {.value = {1, 0, 0, 40, 0, 1, 0, 50, 0, 0, 1, 60}};
    posed.pose.count = 2;
    MeshVertex secondMatrixVertex = vertex;
    secondMatrixVertex.matrixIndex = 1;
    const ClipVertex secondMatrix = transform_vertex(posed, secondMatrixVertex);
    assert(near(secondMatrix.position.x, 82.0F));
    assert(near(secondMatrix.position.y, 156.0F));

    const std::array bindings{
        ModelMatrixBinding{.sourceIndex = 0, .modelView = draw.pose.modelViews[0]},
        ModelMatrixBinding{.sourceIndex = 3, .modelView = posed.pose.modelViews[1]},
    };
    std::array<std::uint8_t, 8> sourceToCompact{};
    ModelPose compactPose{};
    assert(build_model_pose(bindings, compactPose, sourceToCompact) ==
           ModelPoseBuildResult::Success);
    assert(compactPose.count == 2 && sourceToCompact[0] == 0 && sourceToCompact[3] == 1 &&
           sourceToCompact[1] == 0xFF);
    auto duplicateBindings = bindings;
    duplicateBindings[1].sourceIndex = 0;
    assert(build_model_pose(duplicateBindings, compactPose, sourceToCompact) ==
           ModelPoseBuildResult::DuplicateSourceIndex);

    ModelDraw constantColor = draw;
    auto& constantMaterial = std::get<UnlitColorMaterial>(constantColor.material);
    constantMaterial.usesVertexColor = false;
    const ClipVertex constant = transform_vertex(constantColor, vertex);
    assert(constant.color == constantMaterial.baseColor);

    ModelDraw secondaryUv = draw;
    secondaryUv.material = UnlitTexturedMaterial{
        .texture = {.resource = 16, .width = 1, .height = 1},
        .textureCoordinates = ModelTextureCoordinates::Secondary,
    };
    assert(valid(secondaryUv));
    assert(transform_vertex(secondaryUv, vertex).uv == vertex.uv1);

    ModelDraw litColor = draw;
    litColor.material = LitColorMaterial{
        .baseColor = {1.0F, 1.0F, 1.0F, 1.0F},
        .ambientColor = {0.5F, 0.25F, 1.0F, 1.0F},
        .usesVertexRgb = true,
        .usesVertexAlpha = true,
    };
    assert(valid(litColor));
    assert(material_texture_count(litColor.material) == 0);
    const ClipVertex litColorVertex = transform_vertex(litColor, vertex);
    assert(litColorVertex.color == Color(0.25F, 0.0625F, 1.0F, 0.5F));

    ModelDraw lit = draw;
    lit.material = LitTexturedMaterial{
        .texture = {.resource = 17, .width = 1, .height = 1},
        .baseColor = {0.5F, 0.5F, 0.5F, 0.8F},
        .ambientColor = {0.1F, 0.2F, 0.3F, 1.0F},
        .lighting = {.pointLights = {{{.position = {5.0F, 7.0F, 19.0F},
                                       .color = {0.5F, 0.25F, 0.0F, 1.0F}}}},
                     .pointLightCount = 1},
    };
    assert(material_texture(lit.material) == &std::get<LitTexturedMaterial>(lit.material).texture);
    assert(material_texture_count(lit.material) == 1);
    assert(material_texture(lit.material, 1) == nullptr);
    const ClipVertex litVertex = transform_vertex(lit, vertex);
    assert(near(litVertex.color.r, 0.3F));
    assert(near(litVertex.color.g, 0.225F));
    assert(near(litVertex.color.b, 0.15F));
    assert(near(litVertex.color.a, 0.8F));
    assert(litVertex.additiveColor == Color{});

    ModelDraw masked = draw;
    masked.material = AlphaMaskedColorMaterial{
        .texture = {.resource = 19, .width = 1, .height = 1},
        .color = {0.25F, 0.5F, 0.75F, 1.0F},
        .alphaScale = 4.0F,
    };
    assert(material_texture(masked.material) ==
           &std::get<AlphaMaskedColorMaterial>(masked.material).texture);
    const ClipVertex maskedVertex = transform_vertex(masked, vertex);
    assert(maskedVertex.color == (Color{0.0F, 0.0F, 0.0F, 4.0F}));
    assert(maskedVertex.additiveColor == (Color{0.25F, 0.5F, 0.75F, 0.0F}));

    ModelDraw litMask = draw;
    litMask.material = LitTexturedAlphaMaskMaterial{
        .colorTexture = {.resource = 20, .width = 1, .height = 1},
        .alphaMaskTexture = {.resource = 21, .width = 1, .height = 1},
        .baseColor = {0.5F, 0.5F, 0.5F, 1.0F},
        .ambientColor = {0.1F, 0.2F, 0.3F, 1.0F},
        .lighting = {.pointLights = {{{.position = {5.0F, 7.0F, 19.0F},
                                       .color = {0.5F, 0.25F, 0.0F, 1.0F}}}},
                     .pointLightCount = 1},
        .alphaScale = 4.0F,
    };
    assert(material_texture_count(litMask.material) == 2);
    const auto& litMaskMaterial = std::get<LitTexturedAlphaMaskMaterial>(litMask.material);
    assert(material_texture(litMask.material, 0) == &litMaskMaterial.colorTexture);
    assert(material_texture(litMask.material, 1) == &litMaskMaterial.alphaMaskTexture);
    assert(material_texture(litMask.material, 2) == nullptr);
    const ClipVertex litMaskVertex = transform_vertex(litMask, vertex);
    assert(litMaskVertex.uv == vertex.uv);
    assert(litMaskVertex.uv1 == vertex.uv1);
    assert(near(litMaskVertex.color.r, 0.3F));
    assert(near(litMaskVertex.color.g, 0.225F));
    assert(near(litMaskVertex.color.b, 0.15F));
    assert(near(litMaskVertex.color.a, 4.0F));

    // The lighting accumulator saturates before material multiplication. This distinguishes the
    // shipping equation from final-product clamping: 0.5 * clamp(0.8 + 0.8) is 0.5, not 0.8.
    auto& saturatedMaterial = std::get<LitTexturedMaterial>(lit.material);
    saturatedMaterial.ambientColor = {0.8F, 0.8F, 0.8F, 1.0F};
    saturatedMaterial.lighting.pointLights[0].color = {0.8F, 0.8F, 0.8F, 1.0F};
    const ClipVertex saturated = transform_vertex(lit, vertex);
    assert(near(saturated.color.r, 0.5F));
    assert(near(saturated.color.g, 0.5F));
    assert(near(saturated.color.b, 0.5F));

    saturatedMaterial.litColorWeight = 0.5F;
    saturatedMaterial.usesVertexRgb = true;
    saturatedMaterial.usesVertexAlpha = false;
    const ClipVertex halfLit = transform_vertex(lit, vertex);
    assert(near(halfLit.color.r, 0.75F));
    assert(near(halfLit.color.g, 0.625F));
    assert(near(halfLit.color.b, 1.0F));
    assert(near(halfLit.color.a, 0.8F));

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
    invalid = draw;
    std::get<UnlitColorMaterial>(invalid.material).raster.cull = static_cast<ModelCullMode>(0xFF);
    assert(!valid(invalid));
    invalid = lit;
    std::get<LitTexturedMaterial>(invalid.material).litColorWeight = 1.1F;
    assert(!valid(invalid));
    invalid = secondaryUv;
    std::get<UnlitTexturedMaterial>(invalid.material).textureCoordinates =
        static_cast<ModelTextureCoordinates>(0xFF);
    assert(!valid(invalid));
    invalid = litMask;
    std::get<LitTexturedAlphaMaskMaterial>(invalid.material).alphaMaskTexture.resource = 0;
    assert(!valid(invalid));
}
