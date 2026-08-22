#include "runtime/devices/gx_fifo_input.hpp"
#include "runtime/devices/gx_fifo_vertex_layout.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>

namespace {

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

void expectBytes(const GxFifoInput& input, const std::uint8_t* expected, std::size_t size) {
    require(input.size() == size);
    for (std::size_t i = 0; i < size; ++i) {
        require(input.data()[i] == expected[i]);
    }
}

} // namespace

int main() {
    GxFifoInput input;
    input.reserve(8);
    input.setStatsEnabled(true);
    input.appendBigEndian(1, 0xAB);
    input.appendBigEndian(2, 0xCDEF);
    input.appendBigEndian(4, 0x12345678);
    constexpr std::array<std::uint8_t, 7> expected{0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78};
    expectBytes(input, expected.data(), expected.size());

    // Known-difference control: the gather pipe is big-endian, not host/little-endian.
    constexpr std::array<std::uint8_t, 4> littleEndian{0x78, 0x56, 0x34, 0x12};
    bool differs = false;
    for (std::size_t i = 0; i < littleEndian.size(); ++i) {
        differs |= input.data()[3 + i] != littleEndian[i];
    }
    require(differs);

    input.consume(3);
    expectBytes(input, expected.data() + 3, 4);
    input.appendBigEndian(2, 0x9ABC);
    constexpr std::array<std::uint8_t, 6> suffix{0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    expectBytes(input, suffix.data(), suffix.size());

    input.consume(input.size());
    require(input.empty());
    require(input.data() == nullptr);
    input.appendBigEndian(1, 0x55);
    constexpr std::uint8_t finalByte = 0x55;
    expectBytes(input, &finalByte, 1);

    const GxFifoInputStats stats = input.takeStats();
    require(stats.appendCalls == 5);
    require(stats.appendedBytes == 10);
    require(stats.capacityGrowths == 0);
    require(stats.compactions == 1);
    require(stats.compactedBytes == 4);
    const GxFifoInputStats reset = input.takeStats();
    require(reset.appendCalls == 0);

    GxFifoVat direct;
    direct.vcd_lo = (1u << 9) | (1u << 13); // direct position + colour 0
    direct.vcd_hi = 1;                      // direct texcoord 0
    direct.fmt0 = 1u | (4u << 1) | (5u << 14) | (1u << 21) | (2u << 22);
    require(gxFifoVertexSize(direct) == 20); // XYZ-f32 + RGBA8 + ST-u16

    GxFifoVat nbt3;
    nbt3.vcd_lo = (3u << 9) | (2u << 11); // index16 position + index8 normal
    nbt3.fmt0 = 1u << 31;
    require(gxFifoVertexSize(nbt3) == 5);
    nbt3.fmt0 = 0; // known difference: ordinary normal carries one index, not NBT3's three
    require(gxFifoVertexSize(nbt3) == 3);
}
