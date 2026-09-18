#pragma once

// A synthetic big-endian guest image for the title-adapter tests.
//
// It exists so the readers under test are driven through their real reader callback, over memory
// whose contents the test states exactly. Its one deliberate property is that a read outside the
// image answers false rather than zeroes: that is what lets a negative case tell "this address is
// not mapped" apart from "this field is zero", which is the distinction every refusal in these
// readers turns on.

#include <sunbright/title_adapter/guest_memory.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <span>

namespace sb::title_adapter::test {

constexpr GuestAddress RAM_BASE = 0x80000000;
constexpr std::size_t RAM_BYTES = 0x1000;

struct Image {
    std::array<std::uint8_t, RAM_BYTES> bytes{};

    void word(GuestAddress address, std::uint32_t value) {
        const std::size_t offset = address - RAM_BASE;
        assert(offset + 4 <= bytes.size());
        bytes[offset] = static_cast<std::uint8_t>(value >> 24U);
        bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16U);
        bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8U);
        bytes[offset + 3] = static_cast<std::uint8_t>(value);
    }

    void half(GuestAddress address, std::uint16_t value) {
        const std::size_t offset = address - RAM_BASE;
        assert(offset + 2 <= bytes.size());
        bytes[offset] = static_cast<std::uint8_t>(value >> 8U);
        bytes[offset + 1] = static_cast<std::uint8_t>(value);
    }

    void byte(GuestAddress address, std::uint8_t value) {
        const std::size_t offset = address - RAM_BASE;
        assert(offset < bytes.size());
        bytes[offset] = value;
    }

    void real(GuestAddress address, float value) {
        std::uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        word(address, bits);
    }
};

inline bool read_image(GuestAddress address, std::span<std::uint8_t> destination, void* context) {
    const auto& image = *static_cast<const Image*>(context);
    if (address < RAM_BASE) {
        return false;
    }
    const std::uint64_t offset = address - RAM_BASE;
    if (offset > image.bytes.size() || destination.size() > image.bytes.size() - offset) {
        return false;
    }
    std::memcpy(destination.data(), image.bytes.data() + offset, destination.size());
    return true;
}

} // namespace sb::title_adapter::test
