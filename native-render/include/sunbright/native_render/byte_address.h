#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace sb::native_render {

// An address consumed by a semantic asset decoder. Recompiled code supplies a guest address;
// native decomp code supplies a real host pointer. Keeping those representations distinct avoids
// turning native pointers into integers merely to pass through a guest-memory-shaped interface.
class ByteAddress {
  public:
    [[nodiscard]] static constexpr ByteAddress guest(std::uint64_t address) noexcept {
        ByteAddress result;
        result.guestAddress_ = address;
        result.kind_ = Kind::Guest;
        return result;
    }

    [[nodiscard]] static ByteAddress native(const void* address) noexcept {
        ByteAddress result;
        result.nativeAddress_ = static_cast<const std::uint8_t*>(address);
        result.kind_ = Kind::Native;
        return result;
    }

    [[nodiscard]] constexpr bool valid() const noexcept {
        return (kind_ == Kind::Guest && guestAddress_ != 0) ||
               (kind_ == Kind::Native && nativeAddress_ != nullptr);
    }

    [[nodiscard]] constexpr bool guest_value(std::uint64_t& address) const noexcept {
        if (kind_ != Kind::Guest)
            return false;
        address = guestAddress_;
        return true;
    }

    [[nodiscard]] constexpr const std::uint8_t* native_pointer() const noexcept {
        return kind_ == Kind::Native ? nativeAddress_ : nullptr;
    }

    [[nodiscard]] ByteAddress advanced(std::uint64_t bytes) const noexcept {
        if (kind_ == Kind::Guest) {
            if (bytes > std::numeric_limits<std::uint64_t>::max() - guestAddress_)
                return {};
            return guest(guestAddress_ + bytes);
        }
        if (kind_ == Kind::Native && nativeAddress_ != nullptr)
            return native(nativeAddress_ + bytes);
        return {};
    }

    [[nodiscard]] ByteAddress advanced_signed(std::int32_t bytes) const noexcept {
        if (bytes >= 0)
            return advanced(static_cast<std::uint64_t>(bytes));
        const auto magnitude = static_cast<std::uint64_t>(-static_cast<std::int64_t>(bytes));
        if (kind_ == Kind::Guest) {
            if (guestAddress_ < magnitude)
                return {};
            return guest(guestAddress_ - magnitude);
        }
        if (kind_ == Kind::Native && nativeAddress_ != nullptr)
            return native(nativeAddress_ - magnitude);
        return {};
    }

  private:
    enum class Kind : std::uint8_t { Invalid, Guest, Native };

    Kind kind_ = Kind::Invalid;
    std::uint64_t guestAddress_ = 0;
    const std::uint8_t* nativeAddress_ = nullptr;
};

} // namespace sb::native_render
