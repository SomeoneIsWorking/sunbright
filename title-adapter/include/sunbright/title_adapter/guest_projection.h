#pragma once

#include <sunbright/native_render/model.h>
#include <sunbright/title_adapter/guest_memory.h>

#include <cstdint>

namespace sb::title_adapter {

// Reads the projection GMSE01 hands to `GXSetProjection`.
//
// The entry point takes a pointer to a 4x4 in r3 and a projection type in r4, and keeps only six of
// the sixteen values: the two diagonal scales, the two offsets, and the depth row. Which pair it
// takes as the offsets is what the type selects -- column 2 for a perspective projection, column 3
// for an orthographic one -- and the rest of the matrix it discards, because GX supplies those
// entries itself.
//
// This reads all sixteen, because a `native_render::Matrix4x4` is the whole matrix. The ten the
// hardware would have dropped are then checked against what it would have supplied, and a
// disagreement is reported rather than kept: a matrix whose discarded entries are not the canonical
// ones does not mean what the sixteen values say it means, and carrying it through would render
// something the console never drew.
constexpr GuestAddress GMSE01_GX_SET_PROJECTION = 0x80362c34;

enum class GuestProjectionKind : std::uint8_t {
    Perspective,  // GX_PERSPECTIVE: the offsets live in column 2 and the depth row projects w.
    Orthographic, // GX_ORTHOGRAPHIC: the offsets live in column 3 and w is constant.
};

enum class GuestProjectionError : std::uint8_t {
    None,
    NoReader,
    NullMatrix,
    UnreadableMatrix,
    UnknownProjectionType,
    // The matrix disagrees with what the hardware would have supplied for the entries it drops.
    NonCanonicalDiscardedEntries,
};

[[nodiscard]] const char* name(GuestProjectionError error) noexcept;
[[nodiscard]] const char* name(GuestProjectionKind kind) noexcept;

// `matrix` is the guest pointer `GXSetProjection` was given, `type` its second argument.
//
// `out` comes back in the renderer's clip-depth convention, not the console's: the console's
// projections put the near plane at clip z = -w and the far plane at 0, and the renderer's clip
// volume is [0, w]. The conversion belongs to this reader because this is the one place a console
// projection becomes a renderer one, and because the canonical-entry check above has to run on the
// matrix as the title authored it to mean anything.
[[nodiscard]] GuestProjectionError read_guest_projection(const GuestMemory& memory,
                                                         GuestAddress matrix, std::uint32_t type,
                                                         native_render::Matrix4x4& out,
                                                         GuestProjectionKind& kind) noexcept;

} // namespace sb::title_adapter
