#include <sunbright/title_adapter/guest_projection.h>

#include <sunbright/native_render/j3d_projection.h>

#include "guest_image.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
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

// What the reader owes its caller for a matrix the guest wrote: the same sixteen values, in the
// renderer's clip-depth convention. This asks the shipping conversion what that is rather than
// transcribing its result, because what is under test here is that the reader applies it at all --
// that the conversion means near-at-zero and far-at-one is `native_render`'s own test to make.
std::array<float, 16> as_the_renderer_wants_it(const std::array<float, 16>& authored) {
    return sb::native_render::with_zero_to_one_clip_depth({authored}).value;
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
    assert(out.value == as_the_renderer_wants_it(frustum()));

    put(image, ortho());
    assert(read_guest_projection(memory_of(image), MATRIX, GX_ORTHOGRAPHIC, out, kind) ==
           GuestProjectionError::None);
    assert(kind == GuestProjectionKind::Orthographic);
    assert(out.value == as_the_renderer_wants_it(ortho()));
}

// A reader that handed the console's matrix straight through would pass everything above if the
// conversion happened to be the identity, and both of these matrices would still render -- wrongly,
// and only in a way a whole frame shows. So the two must be told apart here.
void the_console_matrix_is_not_what_comes_back() {
    assert(as_the_renderer_wants_it(frustum()) != frustum());
    assert(as_the_renderer_wants_it(ortho()) != ortho());

    Image image{};
    sb::native_render::Matrix4x4 out{};
    GuestProjectionKind kind = GuestProjectionKind::Orthographic;
    put(image, frustum());
    assert(read_guest_projection(memory_of(image), MATRIX, GX_PERSPECTIVE, out, kind) ==
           GuestProjectionError::None);
    assert(out.value != frustum());
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

// `MTXOrtho`'s own arithmetic, so the round trip below is checked against the authored edges
// rather than against numbers copied out of a previous run of the reader.
std::array<float, 16> ortho_of(float top, float bottom, float left, float right) {
    return {2.0F / (right - left),
            0.0F,
            0.0F,
            -(right + left) / (right - left),
            0.0F,
            2.0F / (top - bottom),
            0.0F,
            -(top + bottom) / (top - bottom),
            0.0F,
            0.0F,
            0.0F,
            -1.0F,
            0.0F,
            0.0F,
            0.0F,
            1.0F};
}

bool near(float value, float expected) {
    return std::fabs(value - expected) <= 1.0e-3F;
}

// The screen `TApplication::gameLoop` sets before it fades the frame: it is nobody's
// `J2DGrafContext`, so the projection is the only record of it.
void recovers_the_screen_an_orthographic_projection_states() {
    GuestOrthographicScreen screen{};
    assert(read_orthographic_screen({ortho_of(0.0F, 448.0F, 0.0F, 640.0F)},
                                    GuestProjectionKind::Orthographic,
                                    screen) == GuestOrthographicScreenError::None);
    assert(near(screen.left, 0.0F));
    assert(near(screen.right, 640.0F));
    assert(near(screen.top, 0.0F));
    assert(near(screen.bottom, 448.0F));

    // An off-origin, inverted-vertical screen, so neither edge can be right by being zero and the
    // reader cannot be passing by assuming which way up a console screen is.
    assert(read_orthographic_screen({ortho_of(464.0F, 16.0F, -32.0F, 600.0F)},
                                    GuestProjectionKind::Orthographic,
                                    screen) == GuestOrthographicScreenError::None);
    assert(near(screen.left, -32.0F));
    assert(near(screen.right, 600.0F));
    assert(near(screen.top, 464.0F));
    assert(near(screen.bottom, 16.0F));
}

void refuses_a_projection_that_states_no_screen() {
    GuestOrthographicScreen screen{1.0F, 2.0F, 3.0F, 4.0F};
    const GuestOrthographicScreen untouched = screen;

    // A perspective matrix has offsets in column 2, so the same arithmetic would return numbers;
    // they would just not be a screen.
    assert(read_orthographic_screen({frustum()}, GuestProjectionKind::Perspective, screen) ==
           GuestOrthographicScreenError::NotOrthographic);

    // A collapsed axis: `left == right` makes `MTXOrtho`'s scale infinite, and no pair of edges
    // produced the zero that reaches the reader.
    std::array<float, 16> collapsed = ortho_of(0.0F, 448.0F, 0.0F, 640.0F);
    collapsed[0] = 0.0F;
    assert(read_orthographic_screen({collapsed}, GuestProjectionKind::Orthographic, screen) ==
           GuestOrthographicScreenError::DegenerateScale);
    collapsed = ortho_of(0.0F, 448.0F, 0.0F, 640.0F);
    collapsed[5] = std::numeric_limits<float>::quiet_NaN();
    assert(read_orthographic_screen({collapsed}, GuestProjectionKind::Orthographic, screen) ==
           GuestOrthographicScreenError::DegenerateScale);

    assert(screen.left == untouched.left && screen.top == untouched.top &&
           screen.right == untouched.right && screen.bottom == untouched.bottom);

    for (const GuestOrthographicScreenError error :
         {GuestOrthographicScreenError::None, GuestOrthographicScreenError::NotOrthographic,
          GuestOrthographicScreenError::DegenerateScale}) {
        assert(std::string_view{name(error)} != "unknown");
    }
}

} // namespace

int main() {
    reads_both_projection_types();
    the_console_matrix_is_not_what_comes_back();
    the_type_decides_which_matrix_is_canonical();
    a_non_canonical_entry_is_refused_not_kept();
    names_every_failure();
    recovers_the_screen_an_orthographic_projection_states();
    refuses_a_projection_that_states_no_screen();
    return 0;
}
