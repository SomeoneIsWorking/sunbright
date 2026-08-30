#include <sunbright/native_render/res_timg_decode.h>

#include <sunbright/native_render/image_decode.h>

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
        texture.mipFilter != MipFilter::None) {
        return ResTimgDecodeError::UnsupportedSampler;
    }

    std::size_t encodedBytes = 0;
    std::size_t decodedBytes = 0;
    if (!encoded_image_data_size(texture.width, texture.height, format, encodedBytes) ||
        !decoded_image_data_size(texture.width, texture.height, decodedBytes)) {
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

        const EncodedImageView view{format,        texture.width,  texture.height, encoded,
                                    paletteFormat, paletteEntries, palette};
        DecodedTexture result{};
        result.texture = texture;
        result.rgba8.resize(decodedBytes);
        if (decode_image_rgba8(view, result.rgba8) != ImageDecodeError::None ||
            !image_content_revision(view, result.texture.revision)) {
            return ResTimgDecodeError::DecodeFailure;
        }
        decoded = std::move(result);
        return ResTimgDecodeError::None;
    } catch (const std::bad_alloc&) {
        return ResTimgDecodeError::AllocationFailure;
    }
}

} // namespace sb::native_render
