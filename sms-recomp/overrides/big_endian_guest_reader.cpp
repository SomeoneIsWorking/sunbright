#include "guest_byte_reader.h"

#include <array>
#include <bit>

namespace sb::recomp {

BigEndianGuestReader::BigEndianGuestReader(const GuestByteReader& reader) noexcept
    : reader_(reader) {}

bool BigEndianGuestReader::bytes(std::uint32_t address, void* destination,
                                 std::size_t size) const noexcept {
    return reader_.read != nullptr && reader_.read(reader_.context, address, destination, size);
}

bool BigEndianGuestReader::u8(std::uint32_t address, std::uint8_t& value) const noexcept {
    return bytes(address, &value, sizeof(value));
}

bool BigEndianGuestReader::u16(std::uint32_t address, std::uint16_t& value) const noexcept {
    std::array<std::uint8_t, 2> data{};
    if (!bytes(address, data.data(), data.size()))
        return false;
    value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) | data[1]);
    return true;
}

bool BigEndianGuestReader::u32(std::uint32_t address, std::uint32_t& value) const noexcept {
    std::array<std::uint8_t, 4> data{};
    if (!bytes(address, data.data(), data.size()))
        return false;
    value = (static_cast<std::uint32_t>(data[0]) << 24U) |
            (static_cast<std::uint32_t>(data[1]) << 16U) |
            (static_cast<std::uint32_t>(data[2]) << 8U) | data[3];
    return true;
}

bool BigEndianGuestReader::s32(std::uint32_t address, std::int32_t& value) const noexcept {
    std::uint32_t bits = 0;
    if (!u32(address, bits))
        return false;
    value = std::bit_cast<std::int32_t>(bits);
    return true;
}

bool BigEndianGuestReader::f32(std::uint32_t address, float& value) const noexcept {
    std::uint32_t bits = 0;
    if (!u32(address, bits))
        return false;
    value = std::bit_cast<float>(bits);
    return true;
}

} // namespace sb::recomp
