#pragma once

#include <cstddef>
#include <cstdint>

namespace sb::recomp {

// Reads raw guest bytes without exposing the recomp runtime's memory representation to semantic
// adapters. Tests provide their own implementation; the live runtime uses live_guest_byte_reader().
struct GuestByteReader {
    void* context = nullptr;
    bool (*read)(void* context, std::uint32_t address, void* destination,
                 std::size_t size) = nullptr;
};

[[nodiscard]] GuestByteReader live_guest_byte_reader() noexcept;

} // namespace sb::recomp
