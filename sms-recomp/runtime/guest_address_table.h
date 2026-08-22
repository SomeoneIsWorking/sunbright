#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

// Sparse direct lookup for aligned addresses in GameCube MEM1's cached code window.
//
// The recomp dispatch and native-override registry answer the same hot question: which host object
// owns this exact guest function address? A flat 24 MB / 4-byte table would waste 48 MB per owner;
// a hash table or binary search makes every guest call pay for lookup machinery. This two-level
// table allocates only pages that contain entries while keeping lookup to two indexed loads.
template <typename T> class GuestAddressTable {
  public:
    bool insert(std::uint32_t address, T* value) {
        const auto index = decode(address);
        if (!index.valid || value == nullptr)
            return false;
        auto& page = mPages[index.page];
        if (!page)
            page = std::make_unique<Page>();
        T*& slot = (*page)[index.slot];
        if (slot != nullptr)
            return false;
        slot = value;
        return true;
    }

    [[nodiscard]] T* find(std::uint32_t address) const noexcept {
        const auto index = decode(address);
        if (!index.valid)
            return nullptr;
        const auto& page = mPages[index.page];
        return page ? (*page)[index.slot] : nullptr;
    }

  private:
    static constexpr std::uint32_t kBase = 0x80000000u;
    static constexpr std::uint32_t kSize = 0x01800000u;
    static constexpr std::uint32_t kPageShift = 12;
    static constexpr std::uint32_t kAddressShift = 2;
    static constexpr size_t kPageCount = kSize >> kPageShift;
    static constexpr size_t kSlotsPerPage = 1u << (kPageShift - kAddressShift);

    using Page = std::array<T*, kSlotsPerPage>;

    struct Index {
        size_t page = 0;
        size_t slot = 0;
        bool valid = false;
    };

    static constexpr Index decode(std::uint32_t address) noexcept {
        const std::uint32_t offset = address - kBase;
        if (offset >= kSize || (address & ((1u << kAddressShift) - 1u)) != 0)
            return {};
        return {
            .page = offset >> kPageShift,
            .slot = (offset & ((1u << kPageShift) - 1u)) >> kAddressShift,
            .valid = true,
        };
    }

    std::array<std::unique_ptr<Page>, kPageCount> mPages{};
};
