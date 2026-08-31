#pragma once

#include <sunbright/native_render/byte_address.h>
#include <sunbright/native_render/image.h>
#include <sunbright/native_render/picture.h>

#include <cstdint>
#include <span>
#include <vector>

namespace sb::native_render {

using AssetByteRead = bool (*)(ByteAddress address, std::span<std::uint8_t> output, void* context);

struct AssetByteSource {
    AssetByteRead read = nullptr;
    void* context = nullptr;
};

struct DecodedTexture {
    PictureTexture texture{};
    std::vector<std::uint8_t> rgba8{};
    std::vector<DecodedImageMipLevel> mipLevels{};
};

// Host-order description of one ResTIMG header. The encoded image and palette bytes remain in
// their asset byte order; only these scalar header fields differ after native BMD relocation.
struct ResTimgDescriptor {
    std::uint8_t format = 0;
    bool hasAlpha = false;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t wrapS = 0;
    std::uint8_t wrapT = 0;
    std::uint8_t paletteFormat = 0;
    std::uint16_t paletteEntries = 0;
    std::int32_t paletteOffset = 0;
    std::uint8_t minFilter = 0;
    std::uint8_t magFilter = 0;
    std::uint8_t mipmapCount = 1;
    std::int32_t imageOffset = 0;
};

enum class ResTimgDecodeError : std::uint8_t {
    None,
    InvalidSource,
    HeaderUnreadable,
    UnsupportedFormat,
    UnsupportedSampler,
    InvalidImageOffset,
    ImageUnreadable,
    UnsupportedPalette,
    InvalidPaletteOffset,
    PaletteUnreadable,
    DecodeFailure,
    AllocationFailure,
};

[[nodiscard]] const char* res_timg_decode_error_name(ResTimgDecodeError error) noexcept;

// Decodes every authored image level described by one big-endian ResTIMG resource. Image and
// palette offsets are signed deltas from the header, matching both retail model archives and
// resources relocated by the native decomp loader. No GX object or backend state is accepted here.
[[nodiscard]] ResTimgDecodeError decode_res_timg(const AssetByteSource& source,
                                                 ByteAddress headerAddress,
                                                 std::uint64_t resourceIdentity,
                                                 DecodedTexture& decoded) noexcept;

[[nodiscard]] ResTimgDecodeError decode_res_timg(const AssetByteSource& source,
                                                 const ResTimgDescriptor& descriptor,
                                                 ByteAddress headerAddress,
                                                 std::uint64_t resourceIdentity,
                                                 DecodedTexture& decoded) noexcept;

} // namespace sb::native_render
