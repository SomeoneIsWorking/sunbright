#include "runtime/devices/gx_fifo_contracts.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expression);  \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

int main() {
    using namespace sb::gx_fifo;

    CHECK(checked_mem1_offset(0x817FFFF0u, 16) == 0x017FFFF0u);
    CHECK(!checked_mem1_offset(0x817FFFF0u, 17));
    CHECK(!checked_mem1_offset(0x80000000u, 0));

    CHECK(texture_level_bytes(8, 8, 0) == 32);
    CHECK(texture_level_bytes(4, 4, 6) == 64);
    CHECK(texture_chain_bytes(8, 8, 0, 4) == 128);
    CHECK(!texture_chain_bytes(8, 8, 7, 1));
    CHECK(mip_count(256, 256, 2u << 5, 10u << 12) == 9);
    CHECK(mip_count(256, 256, 0, 10u << 12) == 1);
    CHECK(tlut_bytes_from_entry_count(1) == 2);
    CHECK(tlut_bytes_from_entry_count(16) == 32);
    CHECK(tlut_bytes_from_entry_count(256) == 512);
    CHECK(tlut_bytes_from_entry_count(1024) == 2048);
    CHECK(!tlut_bytes_from_entry_count(0));
    CHECK(!tlut_bytes_from_entry_count(1025));

    CHECK(is_known_opcode(0x00));
    CHECK(is_known_opcode(0x40));
    CHECK(is_known_opcode(0xB8));
    CHECK(!is_known_opcode(0x41));

    std::vector<std::uint8_t> last{1, 2};
    std::vector<std::uint8_t> building;
    rotate_frame(last, building);
    CHECK(last.empty());
    CHECK(building.empty());
    building = {3};
    rotate_frame(last, building);
    CHECK((last == std::vector<std::uint8_t>{3}));
    CHECK(building.empty());
}
