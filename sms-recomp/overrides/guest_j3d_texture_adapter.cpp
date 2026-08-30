#include "guest_j3d_texture_adapter.h"

#include <limits>

namespace sb::recomp {
namespace {

struct ReadContext {
    BigEndianGuestReader reader;
};

bool read_asset(native_render::ByteAddress address, std::span<std::uint8_t> output, void* context) {
    std::uint64_t numericAddress = 0;
    if (!address.guest_value(numericAddress) ||
        numericAddress > std::numeric_limits<std::uint32_t>::max())
        return false;
    return static_cast<ReadContext*>(context)->reader.bytes(
        static_cast<std::uint32_t>(numericAddress), output.data(), output.size());
}

} // namespace

bool capture_guest_j3d_texture(const GuestByteReader& byteReader, std::uint32_t textureTable,
                               std::uint16_t textureNumber, native_render::DecodedTexture& texture,
                               native_render::ResTimgDecodeError& decodeError) noexcept {
    decodeError = native_render::ResTimgDecodeError::InvalidSource;
    const BigEndianGuestReader reader(byteReader);
    std::uint16_t count = 0;
    std::uint32_t resources = 0;
    if (textureTable == 0 || !reader.u16(textureTable, count) || textureNumber >= count ||
        !reader.u32(textureTable + 4, resources) || resources == 0 ||
        textureNumber > (std::numeric_limits<std::uint32_t>::max() - resources) / 0x20U) {
        return false;
    }
    const std::uint32_t header = resources + static_cast<std::uint32_t>(textureNumber) * 0x20U;
    ReadContext context{reader};
    decodeError = native_render::decode_res_timg(
        {read_asset, &context}, native_render::ByteAddress::guest(header),
        (static_cast<std::uint64_t>(textureTable) << 32U) | header, texture);
    return decodeError == native_render::ResTimgDecodeError::None;
}

} // namespace sb::recomp
