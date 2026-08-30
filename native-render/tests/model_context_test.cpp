#include <sunbright/native_render/model_context.h>

#include <cassert>
#include <limits>

int main() {
    using namespace sb::native_render;

    const Matrix4x4 perspective{.value = {2, 0, 0, 0, 0, 3, 0, 0, 0, 0, -0.25F, -2, 0, 0, -1, 0}};
    ModelSceneContext perspectiveContext{};
    assert(capture_j3d_scene_context(perspective, perspectiveContext) ==
           J3dProjectionResult::Perspective);
    assert(perspectiveContext.projectionKind == ProjectionKind::Perspective);
    assert(perspectiveContext.projection.value[10] == -1.25F);
    assert(perspectiveContext.projection.value[11] == -2.0F);

    const Matrix4x4 orthographic{
        .value = {0.5F, 0, 0, -1, 0, 0.5F, 0, 1, 0, 0, -0.25F, -1, 0, 0, 0, 1}};
    ModelSceneContext orthographicContext{};
    assert(capture_j3d_scene_context(orthographic, orthographicContext) ==
           J3dProjectionResult::Orthographic);
    assert(orthographicContext.projectionKind == ProjectionKind::Orthographic);
    assert(orthographicContext.projection.value[10] == -0.25F);
    assert(orthographicContext.projection.value[11] == 0.0F);

    ModelSceneContext unused{};
    Matrix4x4 unsupported = perspective;
    unsupported.value[14] = -0.5F;
    assert(capture_j3d_scene_context(unsupported, unused) == J3dProjectionResult::Unsupported);
    Matrix4x4 nonFinite = perspective;
    nonFinite.value[0] = std::numeric_limits<float>::infinity();
    assert(capture_j3d_scene_context(nonFinite, unused) == J3dProjectionResult::NonFinite);

    ModelSceneContextStack stack{};
    assert(stack.current() == nullptr);
    assert(!stack.pop());
    assert(stack.push(perspectiveContext));
    assert(stack.current()->projectionKind == ProjectionKind::Perspective);
    assert(stack.push(orthographicContext));
    assert(stack.current()->projectionKind == ProjectionKind::Orthographic);
    assert(stack.pop());
    assert(stack.current()->projectionKind == ProjectionKind::Perspective);
    assert(stack.push_empty());
    assert(stack.current() == nullptr);
    assert(stack.pop());
    assert(stack.current()->projectionKind == ProjectionKind::Perspective);
    assert(stack.pop());
    assert(stack.current() == nullptr);

    for (std::size_t index = 0; index < ModelSceneContextStack::kCapacity; ++index)
        assert(stack.push_empty());
    assert(!stack.push_empty());
}
