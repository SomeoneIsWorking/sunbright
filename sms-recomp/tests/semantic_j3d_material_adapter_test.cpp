#include "../overrides/semantic_j3d_material_adapter.h"

#include <array>
#include <bit>
#include <cassert>
#include <cstring>

namespace {

struct Memory {
    std::array<std::uint8_t, 1024> bytes{};
};

bool read(void* context, std::uint32_t address, void* destination, std::size_t size) {
    const auto& memory = *static_cast<const Memory*>(context);
    if (address > memory.bytes.size() || size > memory.bytes.size() - address)
        return false;
    std::memcpy(destination, memory.bytes.data() + address, size);
    return true;
}

void write_u16(Memory& memory, std::size_t address, std::uint16_t value) {
    memory.bytes[address] = static_cast<std::uint8_t>(value >> 8U);
    memory.bytes[address + 1] = static_cast<std::uint8_t>(value);
}

void write_u32(Memory& memory, std::size_t address, std::uint32_t value) {
    memory.bytes[address] = static_cast<std::uint8_t>(value >> 24U);
    memory.bytes[address + 1] = static_cast<std::uint8_t>(value >> 16U);
    memory.bytes[address + 2] = static_cast<std::uint8_t>(value >> 8U);
    memory.bytes[address + 3] = static_cast<std::uint8_t>(value);
}

void write_f32(Memory& memory, std::size_t address, float value) {
    write_u32(memory, address, std::bit_cast<std::uint32_t>(value));
}

} // namespace

int main() {
    constexpr std::uint32_t material = 64;
    constexpr std::uint32_t color = 160;
    constexpr std::uint32_t texgen = 256;
    constexpr std::uint32_t tev = 352;
    constexpr std::uint32_t pixelEngine = 480;
    constexpr std::uint32_t fog = 560;
    Memory memory{};
    write_u32(memory, material + 0x20, color);
    write_u32(memory, material + 0x24, texgen);
    write_u32(memory, material + 0x28, tev);
    write_u32(memory, material + 0x30, pixelEngine);
    write_u32(memory, color, 0x803E0D38);
    write_u32(memory, color + 4, 0x804020FF);
    memory.bytes[color + 0x0C] = 1;
    write_u16(memory, color + 0x0E, 0);
    memory.bytes[color + 0x16] = 2;
    write_u32(memory, texgen, 0x803E0C84);
    write_u32(memory, texgen + 4, 0);
    write_u32(memory, tev, 0x803E0BE8);
    write_u16(memory, tev + 4, 0xFFFF);
    memory.bytes[tev + 6] = 0xFF;
    memory.bytes[tev + 7] = 0xFF;
    memory.bytes[tev + 8] = 4;
    const std::array<std::uint8_t, 8> stage{0xC0, 0x40, 0xAF, 0xF0, 0xC1, 0x08, 0xBF, 0x80};
    std::memcpy(memory.bytes.data() + tev + 0x0A, stage.data(), stage.size());
    write_u32(memory, pixelEngine, 0x803E0E64);

    const sb::recomp::GuestByteReader reader{&memory, read};
    sb::native_render::J3dMaterialState state{};
    assert(sb::recomp::capture_guest_j3d_material_state(reader, material, false, false, state));
    sb::native_render::UnlitColorMaterial output{};
    assert(sb::native_render::classify_j3d_unlit_material(state, output) ==
           sb::native_render::J3dUnlitMaterialResult::Success);
    assert(output.raster.cull == sb::native_render::ModelCullMode::Back);
    assert(output.raster.depthWrite);

    // LightOn and LightOff have different channel offsets. A LightOn-capable block with the
    // channel's lighting enable bit clear is still semantically unlit.
    write_u32(memory, color, 0x803E0CD4);
    memory.bytes[color + 0x14] = 1;
    write_u16(memory, color + 0x16, 0);
    memory.bytes[color + 0x40] = 1;
    assert(sb::recomp::capture_guest_j3d_material_state(reader, material, false, false, state));
    assert(sb::native_render::classify_j3d_unlit_material(state, output) ==
           sb::native_render::J3dUnlitMaterialResult::Success);
    assert(output.raster.cull == sb::native_render::ModelCullMode::Front);

    write_u32(memory, pixelEngine, 0x803E0E00);
    assert(sb::recomp::capture_guest_j3d_material_state(reader, material, false, false, state));
    assert(sb::native_render::classify_j3d_unlit_material(state, output) ==
           sb::native_render::J3dUnlitMaterialResult::Success);
    assert(output.raster.alphaTest == sb::native_render::ModelAlphaTest::GreaterOrEqualHalf);
    write_u32(memory, pixelEngine, 0x803E0D9C);
    assert(sb::recomp::capture_guest_j3d_material_state(reader, material, false, false, state));
    assert(sb::native_render::classify_j3d_unlit_material(state, output) ==
           sb::native_render::J3dUnlitMaterialResult::Success);
    assert(output.raster.blend == sb::native_render::ModelBlendMode::SourceAlpha);
    assert(!output.raster.depthWrite);
    write_u32(memory, pixelEngine, 0x803E0968);
    write_u16(memory, pixelEngine + 0x08, 0x00E7);
    memory.bytes[pixelEngine + 0x0A] = 0;
    memory.bytes[pixelEngine + 0x0B] = 0;
    memory.bytes[pixelEngine + 0x0C] = 0;
    memory.bytes[pixelEngine + 0x0D] = 1;
    memory.bytes[pixelEngine + 0x0E] = 0;
    memory.bytes[pixelEngine + 0x0F] = 3;
    write_u16(memory, pixelEngine + 0x10, 0x0017);
    assert(sb::recomp::capture_guest_j3d_material_state(reader, material, false, false, state));
    assert(sb::native_render::classify_j3d_unlit_material(state, output) ==
           sb::native_render::J3dUnlitMaterialResult::Success);
    assert(output.raster ==
           sb::native_render::ModelRasterPolicy{.cull = sb::native_render::ModelCullMode::Front});
    memory.bytes[pixelEngine + 0x0C] = 2;
    assert(sb::recomp::capture_guest_j3d_material_state(reader, material, false, false, state));
    assert(sb::native_render::classify_j3d_unlit_material(state, output) ==
           sb::native_render::J3dUnlitMaterialResult::UnsupportedRasterPolicy);
    memory.bytes[pixelEngine + 0x0C] = 0;
    write_u32(memory, pixelEngine + 0x04, fog);
    memory.bytes[fog] = 0;
    assert(sb::recomp::capture_guest_j3d_material_state(reader, material, false, false, state));
    assert(sb::native_render::classify_j3d_unlit_material(state, output) ==
           sb::native_render::J3dUnlitMaterialResult::Success);
    memory.bytes[fog] = 2;
    memory.bytes[fog + 1] = 0;
    write_u16(memory, fog + 2, 320);
    write_f32(memory, fog + 4, 300.0F);
    write_f32(memory, fog + 8, 1500.0F);
    write_f32(memory, fog + 12, 1.0F);
    write_f32(memory, fog + 16, 300000.0F);
    write_u32(memory, fog + 20, 0x102030FFU);
    for (std::size_t index = 0; index < state.fog.rangeAdjustmentTable.size(); ++index)
        write_u16(memory, fog + 24 + index * 2, static_cast<std::uint16_t>(100 + index));
    assert(sb::recomp::capture_guest_j3d_material_state(reader, material, false, false, state));
    assert(state.fog.type == 2);
    assert(!state.fog.rangeAdjustmentEnabled);
    assert(state.fog.center == 320);
    assert(state.fog.start == 300.0F);
    assert(state.fog.end == 1500.0F);
    assert(state.fog.near == 1.0F);
    assert(state.fog.far == 300000.0F);
    assert(state.fog.colorRgba8 == 0x102030FFU);
    assert(state.fog.rangeAdjustmentTable[9] == 109);
    assert(sb::native_render::classify_j3d_unlit_material(state, output) ==
           sb::native_render::J3dUnlitMaterialResult::Success);
    sb::native_render::ModelFog modelFog{};
    assert(sb::native_render::build_model_fog(state.fog, modelFog));
    assert(modelFog.mode == sb::native_render::ModelFogMode::Linear);
    assert(modelFog.color == sb::native_render::color_from_rgba8(0x102030FFU));
    memory.bytes[fog] = 4;
    assert(sb::recomp::capture_guest_j3d_material_state(reader, material, false, false, state));
    assert(sb::native_render::classify_j3d_unlit_material(state, output) ==
           sb::native_render::J3dUnlitMaterialResult::UnsupportedRasterPolicy);
    memory.bytes[fog] = 2;
    memory.bytes[fog + 1] = 1;
    assert(sb::recomp::capture_guest_j3d_material_state(reader, material, false, false, state));
    assert(sb::native_render::classify_j3d_unlit_material(state, output) ==
           sb::native_render::J3dUnlitMaterialResult::UnsupportedRasterPolicy);
    memory.bytes[fog + 1] = 0;
    write_u32(memory, pixelEngine + 0x04, 0);
    write_u32(memory, pixelEngine, 0x803E0E64);

    write_u32(memory, tev, 0x803E0B4C);
    write_u16(memory, tev + 0x10, 0x0100);
    write_u16(memory, tev + 0x12, 0xFF80);
    write_u16(memory, tev + 0x14, 0x0040);
    write_u16(memory, tev + 0x16, 0x0020);
    memory.bytes[tev + 0x30] = 1;
    memory.bytes[tev + 0x08] = 0xFF;
    memory.bytes[tev + 0x09] = 0xFF;
    memory.bytes[tev + 0x0A] = 4;
    std::memcpy(memory.bytes.data() + tev + 0x31, stage.data(), stage.size());
    assert(sb::recomp::capture_guest_j3d_material_state(reader, material, false, false, state));
    assert(state.hasTevColor0);
    assert(state.tevColor0S10 == (std::array<std::int16_t, 4>{0x0100, -0x0080, 0x0040, 0x0020}));
    assert(sb::native_render::classify_j3d_unlit_material(state, output) ==
           sb::native_render::J3dUnlitMaterialResult::Success);

    // A TVB2 block owns two texture bindings even when only one TEV stage is active. Stage zero
    // may select slot 1 and secondary texture coordinates; stage count does not reduce the binding
    // table loaded by J3DTevBlock2::load.
    write_u16(memory, tev + 0x04, 0xFFFF);
    write_u16(memory, tev + 0x06, 7);
    memory.bytes[tev + 0x08] = 1;
    memory.bytes[tev + 0x09] = 1;
    memory.bytes[tev + 0x0A] = 4;
    const std::array<std::uint8_t, 8> texturedStage{0xC0, 0x08, 0xF8, 0xAF, 0xC1, 0x08, 0xF2, 0xF0};
    std::memcpy(memory.bytes.data() + tev + 0x31, texturedStage.data(), texturedStage.size());
    write_u32(memory, texgen + 4, 2);
    assert(sb::recomp::capture_guest_j3d_material_state(reader, material, true, false, state));
    assert(state.textureBindings[0].textureNumber == 0xFFFF);
    assert(state.textureBindings[1].textureNumber == 7);
    sb::native_render::UnlitTexturedMaterial texturedOutput{};
    const sb::native_render::PictureTexture placeholder{.resource = 1, .width = 1, .height = 1};
    assert(sb::native_render::classify_j3d_unlit_textured_material(state, placeholder,
                                                                   texturedOutput) ==
           sb::native_render::J3dUnlitTexturedResult::Success);

    write_u16(memory, tev + 0x04, 0xFFFF);
    write_u16(memory, tev + 0x06, 0xFFFF);
    memory.bytes[tev + 0x08] = 0xFF;
    memory.bytes[tev + 0x09] = 0xFF;
    memory.bytes[tev + 0x0A] = 4;
    std::memcpy(memory.bytes.data() + tev + 0x31, stage.data(), stage.size());
    write_u32(memory, texgen + 4, 0);
    memory.bytes[color + 0x14] = 2;
    write_u16(memory, color + 0x1A, 0x1234);
    write_u16(memory, color + 0x1C, 0x5678);
    write_u32(memory, color + 0x08, 0x11223344);
    write_u32(memory, color + 0x10, 0x55667788);
    assert(sb::recomp::capture_guest_j3d_material_state(reader, material, false, false, state));
    assert(state.colorChannelCount == 2);
    assert(state.colorChannelControl1 == 0x1234);
    assert(state.alphaChannelControl1 == 0x5678);
    assert(state.materialColor1Rgba8 == 0x11223344);
    assert(state.ambientColor1Rgba8 == 0x55667788);
    memory.bytes[color + 0x14] = 1;
    memory.bytes[tev + 0x30] = 2;
    write_u32(memory, tev + 0x41, 0x10203040);
    write_u32(memory, tev + 0x45, 0x50607080);
    write_u32(memory, tev + 0x49, 0x90A0B0C0);
    write_u32(memory, tev + 0x4D, 0xD0E0F000);
    memory.bytes[tev + 0x51] = 0x0C;
    memory.bytes[tev + 0x52] = 0x1D;
    memory.bytes[tev + 0x53] = 0x1C;
    memory.bytes[tev + 0x54] = 0x07;
    assert(sb::recomp::capture_guest_j3d_material_state(reader, material, false, false, state));
    assert(state.konstColorRgba8 ==
           (std::array<std::uint32_t, 4>{0x10203040, 0x50607080, 0x90A0B0C0, 0xD0E0F000}));
    assert(state.tevStages[0].konstColorSelection == 0x0C);
    assert(state.tevStages[1].konstColorSelection == 0x1D);
    assert(state.tevStages[0].konstAlphaSelection == 0x1C);
    assert(state.tevStages[1].konstAlphaSelection == 0x07);
    assert(sb::native_render::classify_j3d_unlit_material(state, output) ==
           sb::native_render::J3dUnlitMaterialResult::MultipleTevStages);

    // TV16 has eight texture bindings and sixteen stage slots. Capture every active stage rather
    // than silently truncating a five-stage material to the first two entries.
    write_u32(memory, tev, 0x803E0A14);
    memory.bytes[tev + 0x54] = 5;
    for (std::size_t binding = 0; binding < 8; ++binding)
        write_u16(memory, tev + 0x04 + binding * 2, static_cast<std::uint16_t>(20 + binding));
    for (std::size_t stageIndex = 0; stageIndex < 5; ++stageIndex) {
        memory.bytes[tev + 0x14 + stageIndex * 4] = static_cast<std::uint8_t>(stageIndex % 2);
        memory.bytes[tev + 0x15 + stageIndex * 4] = static_cast<std::uint8_t>(stageIndex + 2);
        memory.bytes[tev + 0x16 + stageIndex * 4] = static_cast<std::uint8_t>(4 + stageIndex);
        const std::array<std::uint8_t, 8> fiveStage{
            0xC0, static_cast<std::uint8_t>(0x10 + stageIndex), 0x80, 0xF0, 0xC1, 0x08,
            0xF0, static_cast<std::uint8_t>(0x80 + stageIndex)};
        std::memcpy(memory.bytes.data() + tev + 0x55 + stageIndex * fiveStage.size(),
                    fiveStage.data(), fiveStage.size());
        memory.bytes[tev + 0x106 + stageIndex] = static_cast<std::uint8_t>(0x03 + stageIndex);
        memory.bytes[tev + 0x116 + stageIndex] = static_cast<std::uint8_t>(0x10 + stageIndex);
    }
    assert(sb::recomp::capture_guest_j3d_material_state(reader, material, false, false, state));
    assert(state.tevStageCount == 5);
    assert(state.textureBindings[7].textureNumber == 27);
    assert(state.tevStages[4].textureCoordinate == 0);
    assert(state.tevStages[4].textureMap == 6);
    assert(state.tevStages[4].colorChannel == 8);
    assert(state.tevStages[4].program ==
           (std::array<std::uint8_t, 8>{0xC0, 0x14, 0x80, 0xF0, 0xC1, 0x08, 0xF0, 0x84}));
    assert(state.tevStages[4].konstColorSelection == 0x07);
    assert(state.tevStages[4].konstAlphaSelection == 0x14);

    write_u32(memory, material + 0x20, 0xFFFF);
    assert(!sb::recomp::capture_guest_j3d_material_state(reader, material, false, false, state));
}
