#include "spc_swap.hpp"

#include <cstdint>
#include <cstring>

namespace {

constexpr std::uint32_t kSpcHeaderSize = 0x1c;
constexpr std::uint32_t kSpcSymbolSize = 0x14;

std::uint32_t read_be32(const unsigned char* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

std::uint32_t read_host32(const unsigned char* data) {
    std::uint32_t value;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

void swap32(unsigned char* data) {
    const unsigned char byte0 = data[0];
    const unsigned char byte1 = data[1];
    data[0] = data[3];
    data[1] = data[2];
    data[2] = byte1;
    data[3] = byte0;
}

} // namespace

SbSpcSwapResult sb_spc_swap_to_host(unsigned char* data) {
    if (data == nullptr || std::memcmp(data, "SPCB", 4) != 0) {
        return SbSpcSwapResult::BadMagic;
    }

    // SPC bytecode begins immediately after its fixed 0x1c-byte header. This
    // invariant distinguishes a fresh big-endian blob from an already-swapped
    // one by content; an address-based cache cannot survive scene-heap reuse.
    const bool big_endian = read_be32(data + 0x04) == kSpcHeaderSize;
    const bool host_endian = read_host32(data + 0x04) == kSpcHeaderSize;
    if (host_endian && !big_endian) {
        return SbSpcSwapResult::AlreadyHostEndian;
    }
    if (!big_endian || host_endian) {
        return SbSpcSwapResult::BadLayout;
    }

    const std::uint32_t data_offset = read_be32(data + 0x08);
    const std::uint32_t data_count = read_be32(data + 0x0c);
    const std::uint32_t symbol_offset = read_be32(data + 0x10);
    const std::uint32_t symbol_count = read_be32(data + 0x14);

    for (std::uint32_t offset = 0x04; offset <= 0x18; offset += 4) {
        swap32(data + offset);
    }
    for (std::uint32_t index = 0; index < symbol_count; ++index) {
        unsigned char* symbol = data + symbol_offset + index * kSpcSymbolSize;
        swap32(symbol + 0x00);
        swap32(symbol + 0x04);
        swap32(symbol + 0x08);
        swap32(symbol + 0x0c);
    }
    for (std::uint32_t index = 0; index < data_count; ++index) {
        swap32(data + data_offset + index * sizeof(std::uint32_t));
    }
    return SbSpcSwapResult::Swapped;
}
