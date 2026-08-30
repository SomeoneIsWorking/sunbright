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

class BigEndianGuestReader {
  public:
    explicit BigEndianGuestReader(const GuestByteReader& reader) noexcept;

    [[nodiscard]] bool bytes(std::uint32_t address, void* destination,
                             std::size_t size) const noexcept;
    [[nodiscard]] bool u8(std::uint32_t address, std::uint8_t& value) const noexcept;
    [[nodiscard]] bool u16(std::uint32_t address, std::uint16_t& value) const noexcept;
    [[nodiscard]] bool u32(std::uint32_t address, std::uint32_t& value) const noexcept;
    [[nodiscard]] bool s32(std::uint32_t address, std::int32_t& value) const noexcept;
    [[nodiscard]] bool f32(std::uint32_t address, float& value) const noexcept;

  private:
    GuestByteReader reader_{};
};

[[nodiscard]] GuestByteReader live_guest_byte_reader() noexcept;

} // namespace sb::recomp
