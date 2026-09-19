#pragma once

#include <sunbright/native_render/image_decode.h>
#include <sunbright/native_render/picture.h>
#include <sunbright/native_render/res_timg_decode.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace sb::native_render {

// Turning one `JUTTexture` into a decoded picture layer.
//
// A J2D pane draws through `JUTTexture`, not through the `ResTIMG` it was built from: `storeTIMG`
// can repoint the object at another resource's pixels, and the palette it samples through is a
// separate `JUTPalette` the resource never names. So the fields here, and not a resource header,
// are what decide what the hardware sampled.
//
// The caller supplies the bytes and nothing else. Where they come from is the only thing that
// differs between a runtime holding the texture as a host object and one copying it out of guest
// memory; everything that decides what those bytes mean -- which formats are owned, which samplers
// are legal, when a palette is required -- is here, so a second reader cannot answer differently
// and render the same pane its own way for a reason no frame could show.

// One `JUTTexture`'s scalar fields, in host order. `paletteFormat` and `paletteEntries` are read
// only when the format is colour-indexed; `hasPalette` says whether the texture named one at all,
// which is what separates "this indexed image has no palette" from "this palette is unusable".
struct JutTextureFields {
    std::uint32_t format = 0;
    bool alphaEnabled = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t wrapS = 0;
    std::uint8_t wrapT = 0;
    std::uint8_t minFilter = 0;
    std::uint8_t magFilter = 0;
    bool hasPalette = false;
    std::uint32_t paletteFormat = 0;
    std::uint32_t paletteEntries = 0;
};

enum class JutTextureError : std::uint8_t {
    None,
    EmptyExtent,
    UnsupportedSampler,
    MipmappedNotImplemented,
    UnsupportedFormat,
    MissingPalette,
    UnsupportedPaletteFormat,
    EncodedBytesTooShort,
    PaletteBytesTooShort,
    AllocationFailure,
    DecodeFailed,
};

[[nodiscard]] const char* jut_texture_error_name(JutTextureError error) noexcept;

// What decoding this texture will consume and produce, and the sampler half of the result.
//
// This is separate from the decode so a caller that has to fetch the bytes knows how many to fetch
// before it fetches them. A caller that already holds them calls `decode_jut_texture` alone.
struct JutTexturePlan {
    PictureTexture texture{};
    EncodedImageFormat format = EncodedImageFormat::Intensity4;
    PaletteFormat paletteFormat = PaletteFormat::Rgb5A3;
    bool indexed = false;
    std::size_t encodedBytes = 0;
    std::size_t paletteBytes = 0;
    std::size_t decodedBytes = 0;
};

[[nodiscard]] JutTextureError plan_jut_texture(const JutTextureFields& fields,
                                               JutTexturePlan& plan) noexcept;

// Decodes the texture to RGBA8. `encoded` and `palette` must be at least the planned sizes; a
// shorter span is refused rather than decoded from whatever follows it.
[[nodiscard]] JutTextureError decode_jut_texture(const JutTextureFields& fields,
                                                 std::span<const std::uint8_t> encoded,
                                                 std::span<const std::uint8_t> palette,
                                                 DecodedTexture& decoded) noexcept;

} // namespace sb::native_render
