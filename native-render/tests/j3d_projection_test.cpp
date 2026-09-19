// Drives the clip-depth conversion against projections built the way the console's own matrix
// library builds them.
//
// The conversion is one line, and a test that restated that line would prove nothing. What is
// asserted instead is the property it exists for: a vertex on the near plane comes out at depth 0
// and one on the far plane at depth 1, for both projection kinds, built from the frustum and
// orthographic formulas rather than from the converted matrix. The negative is asserted too -- the
// unconverted matrix answers -1 and 0 -- because that is the state the renderer was in, and a
// conversion applied twice would pass every positive assertion on its own.

#include <sunbright/native_render/j3d_projection.h>

#include <cassert>
#include <cmath>

namespace {

using sb::native_render::Matrix4x4;
using sb::native_render::with_zero_to_one_clip_depth;

constexpr float NEAR_PLANE = 10.0F;
constexpr float FAR_PLANE = 300000.0F;
constexpr float TOLERANCE = 1.0e-4F;

bool close(float value, float expected) {
    return std::fabs(value - expected) <= TOLERANCE;
}

// C_MTXFrustum, for a symmetric frustum: the depth row is -n/(f-n) and -(f*n)/(f-n), and w is the
// negated eye-space z. These are the exact values the title's own matrix carries, read out of the
// shipping image at 0x80362c34.
Matrix4x4 frustum(float nearPlane, float farPlane) {
    const float span = farPlane - nearPlane;
    Matrix4x4 matrix{};
    matrix.value[0] = 2.04163F;
    matrix.value[5] = 2.74748F;
    matrix.value[10] = -nearPlane / span;
    matrix.value[11] = -(farPlane * nearPlane) / span;
    matrix.value[14] = -1.0F;
    return matrix;
}

// C_MTXOrtho: the depth row is 1/(n-f) and f/(n-f), and w is the constant one. It maps the same two
// planes to the same two clip values as the frustum does, which is what one depth unit serving one
// hardware means.
Matrix4x4 orthographic(float nearPlane, float farPlane) {
    const float span = nearPlane - farPlane;
    Matrix4x4 matrix{};
    matrix.value[0] = 2.0F / 640.0F;
    matrix.value[5] = -2.0F / 448.0F;
    matrix.value[10] = 1.0F / span;
    matrix.value[11] = farPlane / span;
    matrix.value[15] = 1.0F;
    return matrix;
}

// The depth a vertex at `eyeZ` lands on, which is the only thing a pipeline reads: clip z over
// clip w, with w taken from the matrix rather than assumed.
float clip_depth(const Matrix4x4& matrix, float eyeZ) {
    const float z = (matrix.value[10] * eyeZ) + matrix.value[11];
    const float w = (matrix.value[14] * eyeZ) + matrix.value[15];
    return z / w;
}

void converts_a_frustum_to_the_pipelines_range() {
    const Matrix4x4 authored = frustum(NEAR_PLANE, FAR_PLANE);
    // The state this conversion exists to leave: near at -1, far at 0.
    assert(close(clip_depth(authored, -NEAR_PLANE), -1.0F));
    assert(close(clip_depth(authored, -FAR_PLANE), 0.0F));

    const Matrix4x4 converted = with_zero_to_one_clip_depth(authored);
    assert(close(clip_depth(converted, -NEAR_PLANE), 0.0F));
    assert(close(clip_depth(converted, -FAR_PLANE), 1.0F));
    // Halfway in clip depth stays halfway: the conversion is a shift, not a remapping that could
    // reorder two surfaces that were already distinguishable.
    const float middle = -(NEAR_PLANE + FAR_PLANE) / 2.0F;
    assert(close(clip_depth(converted, middle), clip_depth(authored, middle) + 1.0F));
}

void converts_an_orthographic_projection_the_same_way() {
    const Matrix4x4 authored = orthographic(NEAR_PLANE, FAR_PLANE);
    assert(close(clip_depth(authored, -NEAR_PLANE), -1.0F));
    assert(close(clip_depth(authored, -FAR_PLANE), 0.0F));

    const Matrix4x4 converted = with_zero_to_one_clip_depth(authored);
    assert(close(clip_depth(converted, -NEAR_PLANE), 0.0F));
    assert(close(clip_depth(converted, -FAR_PLANE), 1.0F));
}

// Only the depth row moves. A conversion that touched x, y or w would change where the geometry
// lands on the screen as well as how deep it is, and the frame would be wrong in a way no depth
// assertion above would notice.
void leaves_every_other_row_alone() {
    const Matrix4x4 authored = frustum(NEAR_PLANE, FAR_PLANE);
    const Matrix4x4 converted = with_zero_to_one_clip_depth(authored);
    for (std::size_t index = 0; index < 16; ++index) {
        if (index >= 8 && index < 12) {
            continue;
        }
        assert(converted.value[index] == authored.value[index]);
    }
    assert(converted.value[10] != authored.value[10]);
    assert(converted.value[11] == authored.value[11]);
}

// Publishing does not convert, so a projection this renderer composed itself is not shifted twice.
void publishing_does_not_convert() {
    const Matrix4x4 converted = with_zero_to_one_clip_depth(frustum(NEAR_PLANE, FAR_PLANE));
    assert(sb::native_render::current_j3d_projection() == nullptr);
    sb::native_render::publish_j3d_projection(converted);
    const Matrix4x4* published = sb::native_render::current_j3d_projection();
    assert(published != nullptr);
    for (std::size_t index = 0; index < 16; ++index) {
        assert(published->value[index] == converted.value[index]);
    }
    sb::native_render::clear_j3d_projection();
    assert(sb::native_render::current_j3d_projection() == nullptr);
}

} // namespace

int main() {
    converts_a_frustum_to_the_pipelines_range();
    converts_an_orthographic_projection_the_same_way();
    leaves_every_other_row_alone();
    publishing_does_not_convert();
    return 0;
}
