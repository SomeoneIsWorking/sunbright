#include "spc_swap.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

constexpr std::array<unsigned char, 0x48> kBigEndianBlob = {
    'S',  'P',  'C',  'B',  0x00, 0x00, 0x00, 0x1c, // text offset / byte-order discriminator
    0x00, 0x00, 0x00, 0x3c,                         // data-offset table
    0x00, 0x00, 0x00, 0x01,                         // one data entry
    0x00, 0x00, 0x00, 0x20,                         // symbol table
    0x00, 0x00, 0x00, 0x01,                         // one symbol
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, // symbol type
    0x00, 0x00, 0x00, 0x00,                                                 // name offset
    0x00, 0x00, 0x00, 0x07,                                                 // symbol data
    0x00, 0x00, 0x00, 0x00,                                                 // name hash
    0x00, 0x00, 0x00, 0x00,                                                 // native-call slot
    'n',  'a',  'm',  'e',  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, // data offset
};

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

std::uint32_t host32(const unsigned char* data) {
    std::uint32_t value;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

} // namespace

int main() {
    auto storage = kBigEndianBlob;
    require(sb_spc_swap_to_host(storage.data()) == SbSpcSwapResult::Swapped);
    require(host32(storage.data() + 0x04) == 0x1c);
    require(host32(storage.data() + 0x20) == 2);
    require(host32(storage.data() + 0x3c) == 4);
    require(sb_spc_swap_to_host(storage.data()) == SbSpcSwapResult::AlreadyHostEndian);

    // Named defect control: a new archive allocation can reuse the exact same
    // address after scene teardown. Refill the same storage with fresh BE data;
    // the second conversion must not be suppressed by pointer identity.
    storage = kBigEndianBlob;
    require(sb_spc_swap_to_host(storage.data()) == SbSpcSwapResult::Swapped);
    require(host32(storage.data() + 0x14) == 1);

    storage[0] = 'X';
    require(sb_spc_swap_to_host(storage.data()) == SbSpcSwapResult::BadMagic);

    storage = kBigEndianBlob;
    storage[7] = 0x20;
    require(sb_spc_swap_to_host(storage.data()) == SbSpcSwapResult::BadLayout);
}
