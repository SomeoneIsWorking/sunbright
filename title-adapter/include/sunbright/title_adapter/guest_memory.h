#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <span>

namespace sb::title_adapter {

// Reading GMSE01's own data structures out of guest memory.
//
// The guest is 32-bit and big-endian whatever the host is, so every scalar has to be assembled from
// bytes rather than loaded. This is the one owner of that: the readers above it state field offsets
// and meaning, never byte order.

using GuestAddress = std::uint32_t;

// Where the guest's main RAM is mapped in its own address space. Hardware registers state physical
// addresses; every reader here works in the addresses the game's own pointers hold, so the one
// place the two meet says so rather than each caller adding the same constant.
inline constexpr GuestAddress GUEST_RAM_BASE = 0x80000000;

// Reads `destination.size()` bytes of guest memory at `address`. Must answer false when the range
// is not wholly readable: a reader that zero-fills instead turns "this address is not mapped" into
// a structure full of zeroes, which is a legitimate-looking answer to a question that failed.
using GuestReadBytes = bool (*)(GuestAddress address, std::span<std::uint8_t> destination,
                                void* context);

struct GuestMemory {
    GuestReadBytes read = nullptr;
    void* context = nullptr;
};

class GuestReader {
  public:
    explicit GuestReader(const GuestMemory& memory) noexcept : memory_(memory) {}

    [[nodiscard]] bool bytes(GuestAddress address, std::span<std::uint8_t> destination) const {
        return memory_.read(address, destination, memory_.context);
    }

    [[nodiscard]] bool word(GuestAddress address, std::uint32_t& value) const {
        std::array<std::uint8_t, 4> raw{};
        if (!bytes(address, raw)) {
            return false;
        }
        value = (static_cast<std::uint32_t>(raw[0]) << 24) |
                (static_cast<std::uint32_t>(raw[1]) << 16) |
                (static_cast<std::uint32_t>(raw[2]) << 8) | static_cast<std::uint32_t>(raw[3]);
        return true;
    }

    [[nodiscard]] bool half(GuestAddress address, std::uint16_t& value) const {
        std::array<std::uint8_t, 2> raw{};
        if (!bytes(address, raw)) {
            return false;
        }
        value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(raw[0]) << 8) | raw[1]);
        return true;
    }

    [[nodiscard]] bool byte(GuestAddress address, std::uint8_t& value) const {
        return bytes(address, std::span(&value, 1));
    }

    // A guest `f32`. The bit pattern crosses unchanged; only the byte order is undone, so a value
    // the game computed as a NaN stays a NaN here and is refused by the caller that cares rather
    // than being quietly turned into something finite.
    [[nodiscard]] bool real(GuestAddress address, float& value) const {
        std::uint32_t bits = 0;
        if (!word(address, bits)) {
            return false;
        }
        value = std::bit_cast<float>(bits);
        return true;
    }

  private:
    const GuestMemory& memory_;
};

} // namespace sb::title_adapter
