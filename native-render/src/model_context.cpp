#include <sunbright/native_render/model_context.h>

namespace sb::native_render {
namespace {

bool has_projection_row(const Matrix4x4& matrix, float z, float w) noexcept {
    return matrix.value[12] == 0.0F && matrix.value[13] == 0.0F && matrix.value[14] == z &&
           matrix.value[15] == w;
}

} // namespace

J3dProjectionResult capture_j3d_scene_context(const Matrix4x4& gameProjection,
                                              ModelSceneContext& context) noexcept {
    if (!valid(gameProjection))
        return J3dProjectionResult::NonFinite;

    ProjectionKind kind{};
    J3dProjectionResult result{};
    if (has_projection_row(gameProjection, -1.0F, 0.0F)) {
        kind = ProjectionKind::Perspective;
        result = J3dProjectionResult::Perspective;
    } else if (has_projection_row(gameProjection, 0.0F, 1.0F)) {
        kind = ProjectionKind::Orthographic;
        result = J3dProjectionResult::Orthographic;
    } else {
        return J3dProjectionResult::Unsupported;
    }

    context = {.projection = zero_to_one_depth_projection(gameProjection), .projectionKind = kind};
    return result;
}

bool ModelSceneContextStack::push(const ModelSceneContext& context) noexcept {
    if (depth_ == entries_.size() || !valid(context.projection))
        return false;
    entries_[depth_++] = {.context = context, .hasValue = true};
    return true;
}

bool ModelSceneContextStack::push_empty() noexcept {
    if (depth_ == entries_.size())
        return false;
    entries_[depth_++] = {};
    return true;
}

bool ModelSceneContextStack::pop() noexcept {
    if (depth_ == 0)
        return false;
    --depth_;
    return true;
}

const ModelSceneContext* ModelSceneContextStack::current() const noexcept {
    if (depth_ == 0 || !entries_[depth_ - 1].hasValue)
        return nullptr;
    return &entries_[depth_ - 1].context;
}

std::size_t ModelSceneContextStack::depth() const noexcept {
    return depth_;
}

} // namespace sb::native_render
