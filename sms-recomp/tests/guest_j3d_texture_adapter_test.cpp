#include "../overrides/guest_j3d_texture_adapter.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

namespace {

bool read_memory(void* context, std::uint32_t address, void* destination, std::size_t size) {
    const auto& memory = *static_cast<const std::array<std::uint8_t, 512>*>(context);
    if (address > memory.size() || size > memory.size() - address)
        return false;
    std::memcpy(destination, memory.data() + address, size);
    return true;
}

void write_be32(std::array<std::uint8_t, 512>& memory, std::size_t offset, std::uint32_t value) {
    memory[offset] = static_cast<std::uint8_t>(value >> 24U);
    memory[offset + 1] = static_cast<std::uint8_t>(value >> 16U);
    memory[offset + 2] = static_cast<std::uint8_t>(value >> 8U);
    memory[offset + 3] = static_cast<std::uint8_t>(value);
}

} // namespace

int main() {
    std::array<std::uint8_t, 512> memory{};
    constexpr std::size_t table = 0x20;
    constexpr std::size_t header = 0x80;
    memory[table] = 0;
    memory[table + 1] = 1;
    write_be32(memory, table + 4, header);
    memory[header] = 4;
    memory[header + 2] = 0;
    memory[header + 3] = 4;
    memory[header + 4] = 0;
    memory[header + 5] = 4;
    memory[header + 0x14] = 1;
    memory[header + 0x15] = 1;
    write_be32(memory, header + 0x1C, 0x20);
    for (std::size_t index = 0; index < 32; index += 2) {
        memory[header + 0x20 + index] = 0x07;
        memory[header + 0x20 + index + 1] = 0xE0;
    }

    const sb::recomp::GuestByteReader reader{&memory, read_memory};
    sb::native_render::DecodedTexture texture{};
    sb::native_render::ResTimgDecodeError error{};
    assert(sb::recomp::capture_guest_j3d_texture(reader, table, 0, texture, error));
    assert(error == sb::native_render::ResTimgDecodeError::None);
    assert(texture.rgba8[0] == 0 && texture.rgba8[1] == 255 && texture.rgba8[2] == 0);
    assert(!sb::recomp::capture_guest_j3d_texture(reader, table, 1, texture, error));
}
