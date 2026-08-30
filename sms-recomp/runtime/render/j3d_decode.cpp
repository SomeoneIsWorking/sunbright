#include "j3d_decode.h"

#include <lucent/log.h>

#include <cstring>
#include <limits>

namespace {

constexpr u32 kJ3dSys = 0x804045DC;
constexpr u32 kJ3dSysPositionArray = 0x10C;
constexpr u32 kJ3dSysNormalArray = 0x110;
constexpr u32 kJ3dSysColorArray = 0x114;

constexpr u32 kShapeVertexDescriptors = 0x2C;
constexpr u32 kShapeUsesNbt = 0x30;
constexpr u32 kShapeDraws = 0x38;
constexpr u32 kShapeVertexData = 0x44;
constexpr u32 kVertexDataAttributeFormats = 0x0C;
constexpr u32 kVertexDataTextureCoordinate0 = 0x24;
constexpr u32 kShapeDrawDisplayListSize = 0x04;
constexpr u32 kShapeDrawDisplayList = 0x08;

constexpr std::uint32_t kNullAttribute = 0xFF;

bool readable(u32 address, std::size_t bytes = 1) {
    if (bytes == 0 || bytes - 1 > std::numeric_limits<u32>::max() - address)
        return false;
    return sb_ram_fast(address) != nullptr &&
           sb_ram_fast(address + static_cast<u32>(bytes - 1)) != nullptr;
}

bool read_guest_bytes(sb::native_render::ByteAddress address, std::span<std::uint8_t> destination,
                      void*) {
    std::uint64_t numericAddress = 0;
    if (!address.guest_value(numericAddress) || numericAddress > std::numeric_limits<u32>::max() ||
        destination.empty())
        return false;
    const u32 guestAddress = static_cast<u32>(numericAddress);
    if (!readable(guestAddress, destination.size()))
        return false;
    std::memcpy(destination.data(), sb_ram_fast(guestAddress), destination.size());
    return true;
}

} // namespace

bool j3d_build_layout(u32 shape, J3DVertexLayout& layout) {
    layout = {};
    const u32 descriptors = sb_r32(shape + kShapeVertexDescriptors);
    const u32 vertexData = sb_r32(shape + kShapeVertexData);
    if (!readable(descriptors, 8) || !readable(vertexData, kVertexDataAttributeFormats + 4))
        return false;
    const u32 formats = sb_r32(vertexData + kVertexDataAttributeFormats);
    if (!readable(formats, 16))
        return false;

    std::vector<sb::native_render::J3dVertexDescriptor> capturedDescriptors;
    std::vector<sb::native_render::J3dVertexFormat> capturedFormats;

    bool descriptorTerminated = false;
    for (u32 entry = 0; entry < 64; ++entry) {
        const u32 address = descriptors + entry * 8;
        if (!readable(address, 8))
            return false;
        const u32 attribute = sb_r32(address);
        if (attribute == kNullAttribute) {
            descriptorTerminated = true;
            break;
        }
        const u32 type = sb_r32(address + 4);
        capturedDescriptors.push_back({attribute, type});
    }
    if (!descriptorTerminated)
        return false;

    bool formatTerminated = false;
    for (u32 entry = 0; entry < 64; ++entry) {
        const u32 address = formats + entry * 16;
        if (!readable(address, 16))
            return false;
        const u32 attribute = sb_r32(address);
        if (attribute == kNullAttribute) {
            formatTerminated = true;
            break;
        }
        capturedFormats.push_back(
            {attribute, sb_r32(address + 4), sb_r32(address + 8), sb_r8(address + 12)});
    }
    if (!formatTerminated)
        return false;
    return sb::native_render::normalize_j3d_vertex_layout(
        capturedDescriptors, capturedFormats, sb_r8(shape + kShapeUsesNbt) != 0, layout);
}

bool j3d_decode_element(u32 shape, std::uint32_t element, const J3DVertexLayout& layout,
                        std::vector<J3DVert>& output) {
    if (!layout.valid)
        return false;
    const u32 draws = sb_r32(shape + kShapeDraws);
    if (!readable(draws + element * 4, 4))
        return false;
    const u32 draw = sb_r32(draws + element * 4);
    if (!readable(draw, kShapeDrawDisplayList + 4))
        return false;
    const u32 displayList = sb_r32(draw + kShapeDrawDisplayList);
    const u32 displayListSize = sb_r32(draw + kShapeDrawDisplayListSize);

    const u32 vertexData = sb_r32(shape + kShapeVertexData);
    sb::native_render::J3dMeshElementSource source{
        .reader = {read_guest_bytes, nullptr},
        .layout = layout,
        .displayList = sb::native_render::ByteAddress::guest(displayList),
        .displayListSize = displayListSize,
        .positions = sb::native_render::ByteAddress::guest(sb_r32(kJ3dSys + kJ3dSysPositionArray)),
        .normals = sb::native_render::ByteAddress::guest(sb_r32(kJ3dSys + kJ3dSysNormalArray)),
        .colors = sb::native_render::ByteAddress::guest(sb_r32(kJ3dSys + kJ3dSysColorArray)),
    };
    if (readable(vertexData, kVertexDataTextureCoordinate0 + 8 * 4)) {
        for (std::uint32_t set = 0; set < J3D_TEXCOORD_SETS; ++set) {
            source.textureCoordinates[set] = sb::native_render::ByteAddress::guest(
                sb_r32(vertexData + kVertexDataTextureCoordinate0 + set * 4));
        }
    }

    const sb::native_render::J3dMeshDecodeResult result =
        sb::native_render::decode_j3d_mesh_element(source, output);
    if (result.error == sb::native_render::J3dMeshDecodeError::None)
        return true;

    static std::uint32_t reportedFailures = 0;
    if (reportedFailures < 6) {
        ++reportedFailures;
        lucent::error("j3d",
                      "shape 0x{:08x} element {} decode failed at display-list byte {} "
                      "(opcode 0x{:02x}): {}",
                      shape, element, result.displayListOffset, result.opcode,
                      sb::native_render::j3d_mesh_decode_error_name(result.error));
    }
    return false;
}
