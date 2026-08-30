#include <sunbright/native_render/j3d_mesh_decode.h>

#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <span>

namespace {

struct Memory {
    std::array<std::uint8_t, 128> bytes{};
};

bool read(sb::native_render::ByteAddress address, std::span<std::uint8_t> output, void* context) {
    const auto& memory = *static_cast<const Memory*>(context);
    std::uint64_t offset = 0;
    if (!address.guest_value(offset) || offset > memory.bytes.size() ||
        output.size() > memory.bytes.size() - offset)
        return false;
    std::memcpy(output.data(), memory.bytes.data() + offset, output.size());
    return true;
}

void be_float(Memory& memory, std::size_t offset, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    memory.bytes[offset] = static_cast<std::uint8_t>(bits >> 24U);
    memory.bytes[offset + 1] = static_cast<std::uint8_t>(bits >> 16U);
    memory.bytes[offset + 2] = static_cast<std::uint8_t>(bits >> 8U);
    memory.bytes[offset + 3] = static_cast<std::uint8_t>(bits);
}

void native_float(Memory& memory, std::size_t offset, float value) {
    std::memcpy(memory.bytes.data() + offset, &value, sizeof(value));
}

} // namespace

int main() {
    using namespace sb::native_render;
    Memory memory{};
    constexpr std::size_t displayList = 1;
    constexpr std::size_t positions = 32;
    constexpr std::size_t colors = 80;
    memory.bytes[displayList + 0] = 0x90;
    memory.bytes[displayList + 1] = 0;
    memory.bytes[displayList + 2] = 3;
    const std::array<std::uint8_t, 12> payload{0, 0, 0, 0, 3, 0, 1, 1, 6, 0, 2, 2};
    std::memcpy(memory.bytes.data() + displayList + 3, payload.data(), payload.size());
    be_float(memory, positions + 0, 0);
    be_float(memory, positions + 4, 0);
    be_float(memory, positions + 8, 0);
    be_float(memory, positions + 12, 1);
    be_float(memory, positions + 16, 0);
    be_float(memory, positions + 20, 0);
    be_float(memory, positions + 24, 0);
    be_float(memory, positions + 28, 1);
    be_float(memory, positions + 32, 0);
    const std::array<std::uint8_t, 12> rgba{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255};
    std::memcpy(memory.bytes.data() + colors, rgba.data(), rgba.size());

    J3dVertexLayout layout{};
    layout.type[0] = static_cast<std::uint8_t>(J3dAttributeType::Direct);
    layout.type[9] = static_cast<std::uint8_t>(J3dAttributeType::Index16);
    layout.type[11] = static_cast<std::uint8_t>(J3dAttributeType::Index8);
    layout.count[9] = 1;
    layout.component[9] = 4;
    layout.component[11] = 5;
    assert(finalize_j3d_vertex_layout(layout));
    assert(layout.vertexSize == 4);

    const J3dMeshElementSource source{.reader = {read, &memory},
                                      .layout = layout,
                                      .displayList = ByteAddress::guest(displayList),
                                      .displayListSize = 15,
                                      .positions = ByteAddress::guest(positions),
                                      .colors = ByteAddress::guest(colors)};
    std::vector<J3dDecodedVertex> triangles;
    const J3dMeshDecodeResult result = decode_j3d_mesh_element(source, triangles);
    assert(result.error == J3dMeshDecodeError::None);
    assert(triangles.size() == 3);
    assert(triangles[0].positionMatrixSlot == 0);
    assert(triangles[1].positionMatrixSlot == 1);
    assert(triangles[2].positionMatrixSlot == 2);
    assert(triangles[1].x == 1 && triangles[1].y == 0);
    assert(triangles[2].x == 0 && triangles[2].y == 1);
    assert(triangles[0].rgba == 0xFF0000FFU);
    assert(triangles[1].rgba == 0x00FF00FFU);
    assert(triangles[2].rgba == 0x0000FFFFU);

    Memory broken = memory;
    broken.bytes[displayList] = 0x61;
    J3dMeshElementSource brokenSource = source;
    brokenSource.reader.context = &broken;
    triangles.clear();
    const J3dMeshDecodeResult brokenResult = decode_j3d_mesh_element(brokenSource, triangles);
    assert(brokenResult.error == J3dMeshDecodeError::UnknownPrimitive);
    assert(brokenResult.displayListOffset == 0);
    assert(brokenResult.opcode == 0x61);
    assert(triangles.empty());

    // The native decomp loader preserves the big-endian display list but swaps indexed numeric
    // arrays to host order. This planted control must recover both positions and UVs through that
    // mixed representation.
    Memory nativeMemory{};
    constexpr std::size_t nativeTexcoords = 72;
    nativeMemory.bytes[displayList + 0] = 0x90;
    nativeMemory.bytes[displayList + 1] = 0;
    nativeMemory.bytes[displayList + 2] = 3;
    const std::array<std::uint8_t, 12> nativePayload{0, 0, 0, 0, 0, 1, 0, 1, 0, 2, 0, 2};
    std::memcpy(nativeMemory.bytes.data() + displayList + 3, nativePayload.data(),
                nativePayload.size());
    const std::array<float, 9> nativePositions{0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
                                               0.0F, 0.0F, 1.0F, 0.0F};
    for (std::size_t index = 0; index < nativePositions.size(); ++index)
        native_float(nativeMemory, positions + index * sizeof(float), nativePositions[index]);
    const std::array<float, 6> nativeUvs{0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F};
    for (std::size_t index = 0; index < nativeUvs.size(); ++index)
        native_float(nativeMemory, nativeTexcoords + index * sizeof(float), nativeUvs[index]);

    J3dVertexLayout nativeLayout{};
    nativeLayout.type[9] = static_cast<std::uint8_t>(J3dAttributeType::Index16);
    nativeLayout.type[13] = static_cast<std::uint8_t>(J3dAttributeType::Index16);
    nativeLayout.count[9] = 1;
    nativeLayout.count[13] = 1;
    nativeLayout.component[9] = 4;
    nativeLayout.component[13] = 4;
    assert(finalize_j3d_vertex_layout(nativeLayout));
    J3dMeshElementSource nativeSource{.reader = {read, &nativeMemory},
                                      .layout = nativeLayout,
                                      .displayList = ByteAddress::guest(displayList),
                                      .displayListSize = 15,
                                      .positions = ByteAddress::guest(positions),
                                      .textureCoordinates = {ByteAddress::guest(nativeTexcoords)},
                                      .arrayByteOrder = J3dArrayByteOrder::Native};
    triangles.clear();
    assert(decode_j3d_mesh_element(nativeSource, triangles).error == J3dMeshDecodeError::None);
    assert(triangles.size() == 3);
    assert(triangles[1].x == 1.0F && triangles[1].uv[0][0] == 1.0F);
    assert(triangles[2].y == 1.0F && triangles[2].uv[0][1] == 1.0F);

    J3dVertexLayout invalidFraction = layout;
    invalidFraction.fraction[9] = 32;
    assert(!finalize_j3d_vertex_layout(invalidFraction));
}
