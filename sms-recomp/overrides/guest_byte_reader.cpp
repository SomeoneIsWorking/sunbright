#include "guest_byte_reader.h"

#include <intrinsics.h>

#include <cstring>
#include <limits>

namespace sb::recomp {
namespace {

bool read_live_guest_bytes(void*, std::uint32_t address, void* destination,
                           std::size_t size) noexcept {
    if (size == 0)
        return true;
    if (size - 1U > std::numeric_limits<std::uint32_t>::max() - address)
        return false;
    u8* first = sb_ram_fast(address);
    u8* last = sb_ram_fast(address + static_cast<std::uint32_t>(size - 1U));
    if (first == nullptr || last != first + size - 1U)
        return false;
    std::memcpy(destination, first, size);
    return true;
}

} // namespace

GuestByteReader live_guest_byte_reader() noexcept {
    return {.read = read_live_guest_bytes};
}

} // namespace sb::recomp
