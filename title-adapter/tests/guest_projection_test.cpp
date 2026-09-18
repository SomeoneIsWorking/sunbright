#include <sunbright/title_adapter/guest_projection.h>

#include "guest_image.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <string_view>

namespace {

using namespace sb::title_adapter;
using sb::title_adapter::test::Image;
using sb::title_adapter::test::RAM_BASE;
using sb::title_adapter::test::read_image;

constexpr GuestAddress MATRIX = RAM_BASE + 0x100;
constexpr std::uint32_t GX_PERSPECTIVE = 0;
constexpr std::uint32_t GX_ORTHOGRAPHIC = 1;

// What `MTXFrustum` writes: the scales on the diagonal, the offsets in column 2, and a depth row
// that projects w by negating z.
std::array<float, 16> frustum() {
    return {1.5F, 0.0F, 0.25F, 0.0F,  //
            0.0F, 2.0F, -0.5F, 0.0F,  //
            0.0F, 0.0F, -1.1F, -2.2F, //
            0.0F, 0.0F, -1.0F, 0.0F};
}

// What `MTXOrtho` writes: the offsets move to column 3 and w stays constant.
std::array<float, 16> ortho() {
    return {0.003F, 0.0F,    0.0F,  -1.0F, //
            0.0F,   -0.004F, 0.0F,  1.0F,  //
            0.0F,   0.0F,    -1.0F, -1.0F, //
            0.0F,   0.0F,    0.0F,  1.0F};
}

void put(Image& image, const std::array<float, 16>& matrix) {
    for (std::size_t index = 0; index < matrix.size(); ++index) {
        image.real(MATRIX + static_cast<GuestAddress>(index) * 4, matrix[index]);
    }
}

GuestMemory memory_of(Image& image) {
    return {read_image, &image};
}

void reads_both_projection_types() {
    Image image{};
    put(image, frustum());
    sb::native_render::Matrix4x4 out{};
    GuestProjectionKind kind = GuestProjectionKind::Orthographic;
    assert(read_guest_projection(memory_of(image), MATRIX, GX_PERSPECTIVE, out, kind) ==
           GuestProjectionError::None);
    assert(kind == GuestProjectionKind::Perspective);
    // All sixteen come back, in the order the guest wrote them, unswapped by the reader's caller.
    assert(out.value == frustum());

    put(image, ortho());
    assert(read_guest_projection(memory_of(image), MATRIX, GX_ORTHOGRAPHIC, out, kind) ==
           GuestProjectionError::None);
    assert(kind == GuestProjectionKind::Orthographic);
    assert(out.value == ortho());
}

// The two types disagree about which column the offsets live in, so each one's matrix must be
// refused when it is presented as the other. Without this the type argument could be ignored
// entirely and every case above would still pass.
void the_type_decides_which_matrix_is_canonical() {
    Image image{};
    sb::native_render::Matrix4x4 out{};
    GuestProjectionKind kind = GuestProjectionKind::Perspective;
    put(image, frustum());
    assert(read_guest_projection(memory_of(image), MATRIX, GX_ORTHOGRAPHIC, out, kind) ==
           GuestProjectionError::NonCanonicalDiscardedEntries);
    put(image, ortho());
    assert(read_guest_projection(memory_of(image), MATRIX, GX_PERSPECTIVE, out, kind) ==
           GuestProjectionError::NonCanonicalDiscardedEntries);
}

// A matrix the hardware would have partly thrown away must not be carried through as if all
// sixteen of its values meant something.
void a_non_canonical_entry_is_refused_not_kept() {
    Image image{};
    sb::native_render::Matrix4x4 out{};
    GuestProjectionKind kind = GuestProjectionKind::Perspective;
    std::array<float, 16> matrix = frustum();
    matrix[4] = 0.75F; // an entry GX supplies as zero
    put(image, matrix);
    assert(read_guest_projection(memory_of(image), MATRIX, GX_PERSPECTIVE, out, kind) ==
           GuestProjectionError::NonCanonicalDiscardedEntries);
    assert(out.value == sb::native_render::Matrix4x4{}.value);

    matrix = frustum();
    matrix[14] = -0.5F; // the depth row must project w
    put(image, matrix);
    assert(read_guest_projection(memory_of(image), MATRIX, GX_PERSPECTIVE, out, kind) ==
           GuestProjectionError::NonCanonicalDiscardedEntries);
}

void names_every_failure() {
    Image image{};
    put(image, frustum());
    sb::native_render::Matrix4x4 out{};
    GuestProjectionKind kind = GuestProjectionKind::Perspective;
    assert(read_guest_projection({nullptr, nullptr}, MATRIX, GX_PERSPECTIVE, out, kind) ==
           GuestProjectionError::NoReader);
    assert(read_guest_projection(memory_of(image), 0, GX_PERSPECTIVE, out, kind) ==
           GuestProjectionError::NullMatrix);
    assert(read_guest_projection(memory_of(image), MATRIX, 2, out, kind) ==
           GuestProjectionError::UnknownProjectionType);
    assert(read_guest_projection(memory_of(image), RAM_BASE - 0x40, GX_PERSPECTIVE, out, kind) ==
           GuestProjectionError::UnreadableMatrix);
    for (const GuestProjectionError error :
         {GuestProjectionError::None, GuestProjectionError::NoReader,
          GuestProjectionError::NullMatrix, GuestProjectionError::UnreadableMatrix,
          GuestProjectionError::UnknownProjectionType,
          GuestProjectionError::NonCanonicalDiscardedEntries}) {
        assert(std::string_view{name(error)} != "unknown");
    }
    assert(std::string_view{name(GuestProjectionKind::Perspective)} == "perspective");
    assert(std::string_view{name(GuestProjectionKind::Orthographic)} == "orthographic");
}

} // namespace

int main() {
    reads_both_projection_types();
    the_type_decides_which_matrix_is_canonical();
    a_non_canonical_entry_is_refused_not_kept();
    names_every_failure();
    return 0;
}
