#include <sunbright/title_adapter/guest_projection.h>

#include <sunbright/native_render/j3d_projection.h>

#include <cmath>

namespace sb::title_adapter {
namespace {

constexpr std::uint32_t GX_PERSPECTIVE = 0;
constexpr std::uint32_t GX_ORTHOGRAPHIC = 1;
constexpr std::size_t MATRIX_VALUES = 16;
constexpr GuestAddress REAL_BYTES = 4;

// A projection's entries are not near zero by accident, so an exact comparison would reject a
// matrix the hardware would have accepted. This is the tolerance on the entries GX discards, which
// are authored as exact constants by `MTXFrustum` and `MTXOrtho`.
constexpr float TOLERANCE = 1.0e-5F;

[[nodiscard]] bool is(float value, float expected) noexcept {
    return std::fabs(value - expected) <= TOLERANCE;
}

// What GX supplies for itself, per projection type. Index is row * 4 + column.
[[nodiscard]] bool discarded_entries_are_canonical(const native_render::Matrix4x4& matrix,
                                                   GuestProjectionKind kind) noexcept {
    const std::array<float, MATRIX_VALUES>& m = matrix.value;
    const bool shared = is(m[1], 0.0F) && is(m[4], 0.0F) && is(m[8], 0.0F) && is(m[9], 0.0F) &&
                        is(m[12], 0.0F) && is(m[13], 0.0F);
    if (!shared) {
        return false;
    }
    if (kind == GuestProjectionKind::Perspective) {
        // The offsets are in column 2, so column 3 is zero above the depth row, and the depth row
        // projects w by negating z.
        return is(m[3], 0.0F) && is(m[7], 0.0F) && is(m[14], -1.0F) && is(m[15], 0.0F);
    }
    // The offsets are in column 3, so column 2 is zero above the depth row, and w is constant.
    return is(m[2], 0.0F) && is(m[6], 0.0F) && is(m[14], 0.0F) && is(m[15], 1.0F);
}

} // namespace

const char* name(GuestProjectionError error) noexcept {
    switch (error) {
    case GuestProjectionError::None:
        return "none";
    case GuestProjectionError::NoReader:
        return "no reader";
    case GuestProjectionError::NullMatrix:
        return "null matrix";
    case GuestProjectionError::UnreadableMatrix:
        return "unreadable matrix";
    case GuestProjectionError::UnknownProjectionType:
        return "unknown projection type";
    case GuestProjectionError::NonCanonicalDiscardedEntries:
        return "non-canonical discarded entries";
    }
    return "unknown";
}

const char* name(GuestProjectionKind kind) noexcept {
    switch (kind) {
    case GuestProjectionKind::Perspective:
        return "perspective";
    case GuestProjectionKind::Orthographic:
        return "orthographic";
    }
    return "unknown";
}

GuestProjectionError read_guest_projection(const GuestMemory& memory, GuestAddress matrix,
                                           std::uint32_t type, native_render::Matrix4x4& out,
                                           GuestProjectionKind& kind) noexcept {
    if (memory.read == nullptr) {
        return GuestProjectionError::NoReader;
    }
    const GuestReader reader{memory};
    if (matrix == 0) {
        return GuestProjectionError::NullMatrix;
    }
    if (type != GX_PERSPECTIVE && type != GX_ORTHOGRAPHIC) {
        return GuestProjectionError::UnknownProjectionType;
    }
    native_render::Matrix4x4 read{};
    for (std::size_t index = 0; index < MATRIX_VALUES; ++index) {
        if (!reader.real(matrix + static_cast<GuestAddress>(index) * REAL_BYTES,
                         read.value[index])) {
            return GuestProjectionError::UnreadableMatrix;
        }
    }
    const GuestProjectionKind readKind = type == GX_ORTHOGRAPHIC ? GuestProjectionKind::Orthographic
                                                                 : GuestProjectionKind::Perspective;
    if (!discarded_entries_are_canonical(read, readKind)) {
        return GuestProjectionError::NonCanonicalDiscardedEntries;
    }
    // Validated as the console authored it, and handed over in the renderer's clip-depth
    // convention. Both steps are here because this is the one place a guest projection becomes a
    // renderer one: checking a converted matrix against the console's canonical entries would
    // check the conversion rather than the read, and leaving the conversion to each caller is a
    // step a second caller can forget with no symptom but a frame that will not sort.
    out = native_render::with_zero_to_one_clip_depth(read);
    kind = readKind;
    return GuestProjectionError::None;
}

} // namespace sb::title_adapter
