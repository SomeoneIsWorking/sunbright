#include "resource_font_glyph_adapter.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint32_t kBase = 0x1000;

struct Memory {
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x1000);

    static bool read(void* context, std::uint32_t address, void* destination, std::size_t size) {
        auto& self = *static_cast<Memory*>(context);
        if (address < kBase || address - kBase > self.bytes.size() ||
            size > self.bytes.size() - (address - kBase)) {
            return false;
        }
        std::memcpy(destination, self.bytes.data() + (address - kBase), size);
        return true;
    }

    void u8(std::uint32_t address, std::uint8_t value) { bytes[address - kBase] = value; }

    void u16(std::uint32_t address, std::uint16_t value) {
        u8(address, static_cast<std::uint8_t>(value >> 8U));
        u8(address + 1U, static_cast<std::uint8_t>(value));
    }

    void u32(std::uint32_t address, std::uint32_t value) {
        u16(address, static_cast<std::uint16_t>(value >> 16U));
        u16(address + 2U, static_cast<std::uint16_t>(value));
    }
};

} // namespace

int main() {
    using namespace sb;

    constexpr std::uint32_t self = 0x1000;
    constexpr std::uint32_t info = 0x1200;
    constexpr std::uint32_t widthPointers = 0x1300;
    constexpr std::uint32_t widthBlock = 0x1400;
    constexpr std::uint32_t glyphPointers = 0x1500;
    constexpr std::uint32_t glyphBlock = 0x1600;

    Memory memory{};
    memory.u8(self + 0x04, 0);
    memory.u32(self + 0x08, 9);
    memory.u32(self + 0x0c, 0xff0000ff);
    memory.u32(self + 0x10, 0x00ff00ff);
    memory.u32(self + 0x14, 0x0000ffff);
    memory.u32(self + 0x18, 0xffffffff);
    memory.u32(self + 0x1c, 0);
    memory.u32(self + 0x20, 0);
    memory.u32(self + 0x44, 0);
    memory.u32(self + 0x4c, info);
    memory.u32(self + 0x50, widthPointers);
    memory.u32(self + 0x54, glyphPointers);
    memory.u16(self + 0x5c, 1);
    memory.u16(self + 0x62, 0);

    memory.u16(info + 0x0a, 6);
    memory.u16(info + 0x0c, 2);
    memory.u16(info + 0x0e, 8);

    memory.u32(widthPointers, widthBlock);
    memory.u16(widthBlock + 0x08, 0);
    memory.u16(widthBlock + 0x0a, 0);
    memory.u8(widthBlock + 0x0c, 1);
    memory.u8(widthBlock + 0x0d, 6);

    memory.u32(glyphPointers, glyphBlock);
    memory.u16(glyphBlock + 0x08, 0);
    memory.u16(glyphBlock + 0x0a, 0);
    memory.u16(glyphBlock + 0x0c, 8);
    memory.u16(glyphBlock + 0x0e, 8);
    memory.u32(glyphBlock + 0x10, 32);
    memory.u16(glyphBlock + 0x14, 0);
    memory.u16(glyphBlock + 0x16, 1);
    memory.u16(glyphBlock + 0x18, 1);
    memory.u16(glyphBlock + 0x1a, 8);
    memory.u16(glyphBlock + 0x1c, 8);
    for (std::uint32_t index = 0; index < 32; ++index)
        memory.u8(glyphBlock + 0x20 + index, index == 0 ? 0xf0 : 0xff);

    const recomp::GuestByteReader reader{&memory, Memory::read};
    const native_render::Matrix3x4 identity{
        {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}};
    const recomp::ResourceGlyphArgs args{10.0f, 20.0f, 8.0f, 8.0f, 'A', true};
    recomp::CapturedGlyph glyph{};
    assert(
        recomp::capture_resource_font_glyph(reader, self, args, identity, {}, {1, 1, 1, 1}, glyph));
    assert(glyph.command.code == 'A');
    assert(glyph.command.positions[0] == native_render::Vec2(9.0f, 14.0f));
    assert(glyph.command.positions[3] == native_render::Vec2(17.0f, 22.0f));
    assert(glyph.command.atlas.resource == glyphBlock + 0x20);
    assert(glyph.command.atlas.width == 8 && glyph.command.atlas.height == 8);
    assert(glyph.rgba8.size() == 8U * 8U * 4U);
    assert(glyph.image.rgba8.data() == glyph.rgba8.data());
    assert(glyph.rgba8[0] == 0xff && glyph.rgba8[4] == 0x00);

    // Known-negative control: the selected cell must map back into the selected GLY block. This
    // catches a stale or misread post-loadFont state instead of drawing an arbitrary atlas cell.
    memory.u32(self + 0x1c, 8);
    assert(!recomp::capture_resource_font_glyph(reader, self, args, identity, {}, {1, 1, 1, 1},
                                                glyph));
    return 0;
}
