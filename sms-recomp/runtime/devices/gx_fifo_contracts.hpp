#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace sb::gx_fifo {

constexpr std::uint32_t kMem1Size = 0x01800000u;

inline bool is_known_opcode(std::uint8_t op) noexcept {
    return op == 0x00 || op == 0x08 || op == 0x10 || op == 0x20 || op == 0x28 || op == 0x30 ||
           op == 0x38 || op == 0x40 || op == 0x48 || op == 0x50 || op == 0x61 ||
           (op >= 0x80 && op <= 0xBF);
}

inline std::optional<std::uint32_t> checked_mem1_offset(std::uint32_t guestAddress,
                                                        std::size_t byteCount) noexcept {
    if (byteCount == 0)
        return std::nullopt;
    const std::uint32_t offset = guestAddress & 0x01FFFFFFu;
    if (offset >= kMem1Size || byteCount > kMem1Size - offset)
        return std::nullopt;
    return offset;
}

inline std::optional<std::size_t> texture_level_bytes(std::uint32_t width, std::uint32_t height,
                                                      std::uint32_t format) noexcept {
    if (width == 0 || height == 0)
        return std::nullopt;
    std::uint32_t blockWidth = 0;
    std::uint32_t blockHeight = 0;
    std::uint32_t blockBytes = 0;
    switch (format) {
    case 0:  // I4
    case 8:  // C4
    case 14: // CMPR
        blockWidth = 8;
        blockHeight = 8;
        blockBytes = 32;
        break;
    case 1: // I8
    case 2: // IA4
    case 9: // C8
        blockWidth = 8;
        blockHeight = 4;
        blockBytes = 32;
        break;
    case 3:  // IA8
    case 4:  // RGB565
    case 5:  // RGB5A3
    case 10: // C14X2
        blockWidth = 4;
        blockHeight = 4;
        blockBytes = 32;
        break;
    case 6: // RGBA8
        blockWidth = 4;
        blockHeight = 4;
        blockBytes = 64;
        break;
    default:
        return std::nullopt;
    }
    const std::size_t xBlocks = (width + blockWidth - 1) / blockWidth;
    const std::size_t yBlocks = (height + blockHeight - 1) / blockHeight;
    if (xBlocks > std::numeric_limits<std::size_t>::max() / yBlocks)
        return std::nullopt;
    const std::size_t blocks = xBlocks * yBlocks;
    if (blocks > std::numeric_limits<std::size_t>::max() / blockBytes)
        return std::nullopt;
    return blocks * blockBytes;
}

inline std::optional<std::size_t> texture_chain_bytes(std::uint32_t width, std::uint32_t height,
                                                      std::uint32_t format,
                                                      std::uint32_t mipCount) noexcept {
    if (mipCount == 0)
        return std::nullopt;
    std::size_t total = 0;
    for (std::uint32_t level = 0; level < mipCount; ++level) {
        const auto bytes = texture_level_bytes(std::max(width >> level, 1u),
                                               std::max(height >> level, 1u), format);
        if (!bytes || *bytes > std::numeric_limits<std::size_t>::max() - total)
            return std::nullopt;
        total += *bytes;
    }
    return total;
}

inline std::uint32_t mip_count(std::uint32_t width, std::uint32_t height, std::uint32_t mode0,
                               std::uint32_t mode1) noexcept {
    const std::uint32_t minFilter = (mode0 >> 5) & 7u;
    if (minFilter < 2u)
        return 1;
    const std::uint32_t byLod = ((mode1 >> 8) & 0xFFu) / 16u + 1u;
    std::uint32_t dimension = std::max(width, height);
    std::uint32_t byDimension = 1;
    while (dimension > 1) {
        dimension >>= 1;
        ++byDimension;
    }
    return std::min(byLod, byDimension);
}

inline std::optional<std::size_t> tlut_bytes_from_entry_count(std::uint32_t entryCount) noexcept {
    if (entryCount == 0 || entryCount > 1024)
        return std::nullopt;
    return static_cast<std::size_t>(entryCount) * sizeof(std::uint16_t);
}

template <typename ByteVector> void rotate_frame(ByteVector& last, ByteVector& building) {
    last.swap(building);
    building.clear();
}

} // namespace sb::gx_fifo
