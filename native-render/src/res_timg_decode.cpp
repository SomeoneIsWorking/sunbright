#include <sunbright/native_render/res_timg_decode.h>

#include <sunbright/native_render/image_decode.h>

#include <algorithm>
#include <array>
#include <new>
#include <utility>

namespace sb::native_render {
namespace {

std::uint16_t be16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) | bytes[1]);
}

std::uint32_t be32(const std::uint8_t* bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) | bytes[3];
}

bool read(const AssetByteSource& source, ByteAddress address,
          std::span<std::uint8_t> output) noexcept {
    return source.read != nullptr && address.valid() &&
           source.read(address, output, source.context);
}

constexpr std::uint64_t kRevisionOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kRevisionPrime = 1099511628211ULL;

void mix_revision_byte(std::uint64_t& revision, std::uint8_t byte) noexcept {
    revision ^= byte;
    revision *= kRevisionPrime;
}

void mix_revision_u64(std::uint64_t& revision, std::uint64_t value) noexcept {
    for (std::uint32_t shift = 0; shift != 64; shift += 8)
        mix_revision_byte(revision, static_cast<std::uint8_t>(value >> shift));
}

} // namespace

const char* res_timg_decode_error_name(ResTimgDecodeError error) noexcept {
    switch (error) {
    case ResTimgDecodeError::None:
        return "none";
    case ResTimgDecodeError::InvalidSource:
        return "invalid source";
    case ResTimgDecodeError::HeaderUnreadable:
        return "header unreadable";
    case ResTimgDecodeError::UnsupportedFormat:
        return "unsupported format";
    case ResTimgDecodeError::UnsupportedSampler:
        return "unsupported sampler";
    case ResTimgDecodeError::InvalidImageOffset:
        return "invalid image offset";
    case ResTimgDecodeError::ImageUnreadable:
        return "image unreadable";
    case ResTimgDecodeError::UnsupportedPalette:
        return "unsupported palette";
    case ResTimgDecodeError::InvalidPaletteOffset:
        return "invalid palette offset";
    case ResTimgDecodeError::PaletteUnreadable:
        return "palette unreadable";
    case ResTimgDecodeError::DecodeFailure:
        return "image decode failure";
    case ResTimgDecodeError::AllocationFailure:
        return "allocation failure";
    }
    return "unknown";
}

ResTimgDecodeError decode_res_timg(const AssetByteSource& source, ByteAddress headerAddress,
                                   std::uint64_t resourceIdentity,
                                   DecodedTexture& decoded) noexcept {
    if (source.read == nullptr || !headerAddress.valid() || resourceIdentity == 0)
        return ResTimgDecodeError::InvalidSource;
    std::array<std::uint8_t, 0x20> header{};
    if (!read(source, headerAddress, header))
        return ResTimgDecodeError::HeaderUnreadable;

    const ResTimgDescriptor descriptor{
        .format = header[0],
        .hasAlpha = header[0x01] != 0,
        .width = be16(header.data() + 0x02),
        .height = be16(header.data() + 0x04),
        .wrapS = header[0x06],
        .wrapT = header[0x07],
        .paletteFormat = header[0x09],
        .paletteEntries = be16(header.data() + 0x0A),
        .paletteOffset = static_cast<std::int32_t>(be32(header.data() + 0x0C)),
        .minFilter = header[0x14],
        .magFilter = header[0x15],
        .mipmapCount = header[0x18],
        .imageOffset = static_cast<std::int32_t>(be32(header.data() + 0x1C)),
    };
    return decode_res_timg(source, descriptor, headerAddress, resourceIdentity, decoded);
}

ResTimgDecodeError decode_res_timg(const AssetByteSource& source,
                                   const ResTimgDescriptor& descriptor, ByteAddress headerAddress,
                                   std::uint64_t resourceIdentity,
                                   DecodedTexture& decoded) noexcept {
    if (source.read == nullptr || !headerAddress.valid() || resourceIdentity == 0)
        return ResTimgDecodeError::InvalidSource;

    EncodedImageFormat format{};
    if (!decode_image_format(descriptor.format, format))
        return ResTimgDecodeError::UnsupportedFormat;
    PictureTexture texture{};
    texture.resource = resourceIdentity;
    texture.width = descriptor.width;
    texture.height = descriptor.height;
    texture.hasAlpha = descriptor.hasAlpha;
    if (!decode_address_mode(descriptor.wrapS, texture.addressU) ||
        !decode_address_mode(descriptor.wrapT, texture.addressV) ||
        !decode_min_filter(descriptor.minFilter, texture.minFilter, texture.mipFilter) ||
        !decode_mag_filter(descriptor.magFilter, texture.magFilter) ||
        descriptor.mipmapCount == 0 ||
        (texture.mipFilter != MipFilter::None && descriptor.mipmapCount == 1)) {
        return ResTimgDecodeError::UnsupportedSampler;
    }

    std::size_t encodedBytes = 0;
    if (!encoded_image_chain_size(texture.width, texture.height, format, descriptor.mipmapCount,
                                  encodedBytes)) {
        return ResTimgDecodeError::UnsupportedFormat;
    }
    const ByteAddress imageAddress = headerAddress.advanced_signed(descriptor.imageOffset);
    if (!imageAddress.valid())
        return ResTimgDecodeError::InvalidImageOffset;

    try {
        std::vector<std::uint8_t> encoded(encodedBytes);
        if (!read(source, imageAddress, encoded))
            return ResTimgDecodeError::ImageUnreadable;

        PaletteFormat paletteFormat = PaletteFormat::Rgb5A3;
        std::uint32_t paletteEntries = 0;
        std::vector<std::uint8_t> palette;
        if (format == EncodedImageFormat::Indexed4 || format == EncodedImageFormat::Indexed8 ||
            format == EncodedImageFormat::Indexed14) {
            paletteEntries = descriptor.paletteEntries;
            if (paletteEntries == 0 ||
                !decode_palette_format(descriptor.paletteFormat, paletteFormat))
                return ResTimgDecodeError::UnsupportedPalette;
            const ByteAddress paletteAddress =
                headerAddress.advanced_signed(descriptor.paletteOffset);
            if (!paletteAddress.valid())
                return ResTimgDecodeError::InvalidPaletteOffset;
            palette.resize(static_cast<std::size_t>(paletteEntries) * 2U);
            if (!read(source, paletteAddress, palette))
                return ResTimgDecodeError::PaletteUnreadable;
        }

        DecodedTexture result{};
        result.texture = texture;
        result.mipLevels.reserve(descriptor.mipmapCount - 1U);
        std::size_t encodedOffset = 0;
        std::uint32_t levelWidth = texture.width;
        std::uint32_t levelHeight = texture.height;
        std::uint64_t revision = kRevisionOffsetBasis;
        mix_revision_u64(revision, descriptor.mipmapCount);
        for (std::uint32_t level = 0; level < descriptor.mipmapCount; ++level) {
            std::size_t levelEncodedBytes = 0;
            std::size_t levelDecodedBytes = 0;
            if (!encoded_image_data_size(levelWidth, levelHeight, format, levelEncodedBytes) ||
                !decoded_image_data_size(levelWidth, levelHeight, levelDecodedBytes) ||
                levelEncodedBytes > encoded.size() - encodedOffset) {
                return ResTimgDecodeError::DecodeFailure;
            }
            const EncodedImageView view{
                format,        levelWidth,
                levelHeight,   std::span(encoded).subspan(encodedOffset, levelEncodedBytes),
                paletteFormat, paletteEntries,
                palette};
            std::uint64_t levelRevision = 0;
            if (!image_content_revision(view, levelRevision))
                return ResTimgDecodeError::DecodeFailure;
            mix_revision_u64(revision, levelWidth);
            mix_revision_u64(revision, levelHeight);
            mix_revision_u64(revision, levelRevision);
            if (level == 0) {
                result.rgba8.resize(levelDecodedBytes);
                if (decode_image_rgba8(view, result.rgba8) != ImageDecodeError::None)
                    return ResTimgDecodeError::DecodeFailure;
            } else {
                DecodedImageMipLevel& decodedLevel = result.mipLevels.emplace_back();
                decodedLevel.width = levelWidth;
                decodedLevel.height = levelHeight;
                decodedLevel.rgba8.resize(levelDecodedBytes);
                if (decode_image_rgba8(view, decodedLevel.rgba8) != ImageDecodeError::None)
                    return ResTimgDecodeError::DecodeFailure;
            }
            encodedOffset += levelEncodedBytes;
            levelWidth = std::max(levelWidth >> 1U, 1U);
            levelHeight = std::max(levelHeight >> 1U, 1U);
        }
        result.texture.revision = revision;
        decoded = std::move(result);
        return ResTimgDecodeError::None;
    } catch (const std::bad_alloc&) {
        return ResTimgDecodeError::AllocationFailure;
    }
}

} // namespace sb::native_render
