#include <sunbright/native_render/res_timg_decode.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <span>

namespace {

bool read_memory(sb::native_render::ByteAddress address, std::span<std::uint8_t> output,
                 void* context) {
    const auto& memory = *static_cast<const std::array<std::uint8_t, 256>*>(context);
    std::uint64_t offset = 0;
    if (!address.guest_value(offset) || offset > memory.size() ||
        output.size() > memory.size() - offset)
        return false;
    std::copy_n(memory.data() + offset, output.size(), output.data());
    return true;
}

} // namespace

int main() {
    using namespace sb::native_render;
    std::array<std::uint8_t, 256> memory{};
    constexpr std::size_t header = 0x40;
    memory[header] = 4; // RGB565, 4x4 tile = 32 bytes.
    memory[header + 2] = 0;
    memory[header + 3] = 4;
    memory[header + 4] = 0;
    memory[header + 5] = 4;
    memory[header + 6] = 1;
    memory[header + 7] = 2;
    memory[header + 0x14] = 1;
    memory[header + 0x15] = 1;
    memory[header + 0x1F] = 0x20;
    for (std::size_t index = 0; index < 32; index += 2) {
        memory[header + 0x20 + index] = 0xF8;
        memory[header + 0x20 + index + 1] = 0x00;
    }

    const AssetByteSource source{read_memory, &memory};
    DecodedTexture decoded{};
    assert(decode_res_timg(source, ByteAddress::guest(header), 99, decoded) ==
           ResTimgDecodeError::None);
    assert(decoded.texture.resource == 99);
    assert(decoded.texture.width == 4 && decoded.texture.height == 4);
    assert(decoded.texture.addressU == AddressMode::Repeat);
    assert(decoded.texture.addressV == AddressMode::Mirror);
    assert(decoded.rgba8.size() == 64);
    assert(decoded.rgba8[0] == 255 && decoded.rgba8[1] == 0 && decoded.rgba8[2] == 0 &&
           decoded.rgba8[3] == 255);

    // The native BMD loader has already byte-swapped the ResTIMG header. Its normalized
    // description must decode the same untouched big-endian texel payload.
    const ResTimgDescriptor hostDescriptor{.format = 4,
                                           .width = 4,
                                           .height = 4,
                                           .wrapS = 1,
                                           .wrapT = 2,
                                           .minFilter = 1,
                                           .magFilter = 1,
                                           .imageOffset = 0x20};
    DecodedTexture nativeDecoded{};
    assert(decode_res_timg(source, hostDescriptor, ByteAddress::guest(header), 100,
                           nativeDecoded) == ResTimgDecodeError::None);
    assert(nativeDecoded.texture.resource == 100);
    assert(nativeDecoded.rgba8 == decoded.rgba8);

    memory[header + 0x14] = 5;
    assert(decode_res_timg(source, ByteAddress::guest(header), 99, decoded) ==
           ResTimgDecodeError::UnsupportedSampler);
}
