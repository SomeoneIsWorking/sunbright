#pragma once

#include <sunbright/title_adapter/guest_memory.h>

#include <cstdint>

namespace sb::title_adapter {

// `JUTTexture`, and the `JUTPalette` an indexed one samples through, read out of guest memory.
//
// Two J2D readers need exactly this and neither is reachable from the other: a `J2DPicture` holds
// up to four of them in a layer array, a `J2DWindow` holds five in named roles, and both hand the
// same fields to `native_render::decode_jut_texture`. One owner, so the two cannot drift about what
// a texture object says.

// The encoded image is read through these and not through the `ResTIMG` at 0x20: `storeTIMG` can
// repoint the object at another resource's pixels, and `JUTTexture` is what `load` hands GX. The
// resource address is carried out all the same, as the identity a decoded image is cached under.
inline constexpr GuestAddress GUEST_JUT_TEXTURE_RESOURCE = 0x20;
inline constexpr GuestAddress GUEST_JUT_TEXTURE_DATA = 0x24;
inline constexpr GuestAddress GUEST_JUT_TEXTURE_ACTIVE_PALETTE = 0x2C;
inline constexpr GuestAddress GUEST_JUT_TEXTURE_FORMAT = 0x34;
inline constexpr GuestAddress GUEST_JUT_TEXTURE_ALPHA_ENABLED = 0x38;
inline constexpr GuestAddress GUEST_JUT_TEXTURE_WIDTH = 0x3C;
inline constexpr GuestAddress GUEST_JUT_TEXTURE_HEIGHT = 0x3E;
inline constexpr GuestAddress GUEST_JUT_TEXTURE_WRAP_S = 0x40;
inline constexpr GuestAddress GUEST_JUT_TEXTURE_WRAP_T = 0x41;
inline constexpr GuestAddress GUEST_JUT_TEXTURE_MIN_FILTER = 0x42;
inline constexpr GuestAddress GUEST_JUT_TEXTURE_MAG_FILTER = 0x43;

// `JUTPalette`, past the 12-byte `GXTlutObj` it opens with.
inline constexpr GuestAddress GUEST_JUT_PALETTE_FORMAT = 0x10;
inline constexpr GuestAddress GUEST_JUT_PALETTE_COLOR_TABLE = 0x14;
inline constexpr GuestAddress GUEST_JUT_PALETTE_ENTRIES = 0x18;

enum class GuestJutTextureError : std::uint8_t {
    None,
    NullTexture,
    UnreadableTexture,
    UnreadablePalette,
    NullPaletteColorTable,
};

[[nodiscard]] const char* name(GuestJutTextureError error) noexcept;

// The colour table one indexed texture samples through. Read whenever the texture names one,
// because whether a format is indexed is the image decoder's question and not this reader's.
struct GuestJutPalette {
    GuestAddress address = 0;
    GuestAddress colorTable = 0;
    std::uint32_t format = 0;
    std::uint16_t entries = 0;

    [[nodiscard]] bool present() const noexcept { return address != 0; }
};

struct GuestJutTexture {
    GuestAddress address = 0;
    GuestAddress resource = 0;
    GuestAddress data = 0;
    std::uint32_t format = 0;
    std::uint32_t alphaEnabled = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t wrapS = 0;
    std::uint8_t wrapT = 0;
    std::uint8_t minFilter = 0;
    std::uint8_t magFilter = 0;
    GuestJutPalette palette{};
};

[[nodiscard]] GuestJutTextureError read_guest_jut_texture(const GuestReader& reader,
                                                          GuestAddress texture,
                                                          GuestJutTexture& out) noexcept;

} // namespace sb::title_adapter
