#include <sunbright/title_adapter/guest_j3d_shape.h>

#include <array>

namespace sb::title_adapter {
namespace {

// Retail J3DShape, from decomp/sms/include/JSystem/J3D/J3DGraphBase/J3DShape.hpp.
constexpr GuestAddress SHAPE_INDEX = 0x04;
constexpr GuestAddress SHAPE_ELEMENT_COUNT = 0x06;
constexpr GuestAddress SHAPE_FLAGS = 0x08;
constexpr GuestAddress SHAPE_VTX_DESC_LIST = 0x2c;
constexpr GuestAddress SHAPE_NBT = 0x30;
constexpr GuestAddress SHAPE_DRAWS = 0x38;
constexpr GuestAddress SHAPE_VERTEX_DATA = 0x44;

// Retail J3DVertexData, from decomp/sms/include/JSystem/J3D/J3DGraphBase/J3DVertex.hpp.
constexpr GuestAddress VERTEX_DATA_VTX_NUM = 0x00;
constexpr GuestAddress VERTEX_DATA_NRM_NUM = 0x04;
constexpr GuestAddress VERTEX_DATA_COL_NUM = 0x08;
constexpr GuestAddress VERTEX_DATA_ATTR_FMT_LIST = 0x0c;
constexpr GuestAddress VERTEX_DATA_TEX_COORD_ARRAY = 0x24;

// Retail J3DSys, from decomp/sms/include/JSystem/J3D/J3DGraphBase/J3DSys.hpp and confirmed against
// J3DShape::loadVtxArray's own reads at 0x802e0344/0x802e0358/0x802e036c.
constexpr GuestAddress SYSTEM_VTX_POS = 0x10c;
constexpr GuestAddress SYSTEM_VTX_NRM = 0x110;
constexpr GuestAddress SYSTEM_VTX_COL = 0x114;

// Retail J3DShapeDraw. It declares a virtual destructor, so the guest object opens with its vtable
// pointer and the two data members follow it.
constexpr GuestAddress SHAPE_DRAW_DISPLAY_LIST_SIZE = 0x04;
constexpr GuestAddress SHAPE_DRAW_DISPLAY_LIST = 0x08;

// GXVtxDescList is two 32-bit enums; GXVtxAttrFmtList is three plus a u8 that pads to a word. Both
// lists end at an entry whose attribute is GX_VA_NULL rather than carrying a count.
constexpr std::uint32_t VTX_DESC_ENTRY_BYTES = 8;
constexpr std::uint32_t VTX_ATTR_FMT_ENTRY_BYTES = 16;
constexpr std::uint32_t GX_VA_NULL = 0xff;

// Both lists are authored by the model loader and are short. The cap is what makes an unterminated
// list a named failure instead of a read that walks guest memory until it faults: it is
// deliberately far above any real list, so reaching it means the list is not a list.
constexpr std::size_t MAX_LIST_ENTRIES = 64;

// Reads the descriptor list and the attribute-format list into native_render's own input types.
// Neither loop can run away: each stops at GX_VA_NULL, and reaching MAX_LIST_ENTRIES without one is
// reported rather than silently truncated, because a truncated vertex description produces a
// plausible mesh with the wrong stride.
[[nodiscard]] GuestShapeError
read_descriptors(const GuestReader& reader, GuestAddress list,
                 std::array<native_render::J3dVertexDescriptor, MAX_LIST_ENTRIES>& descriptors,
                 std::uint32_t& count) {
    for (std::size_t entry = 0; entry < MAX_LIST_ENTRIES; ++entry) {
        const GuestAddress address =
            list + static_cast<std::uint32_t>(entry) * VTX_DESC_ENTRY_BYTES;
        std::uint32_t attribute = 0;
        std::uint32_t type = 0;
        if (!reader.word(address, attribute) || !reader.word(address + 4, type)) {
            return GuestShapeError::UnreadableDescriptorList;
        }
        if (attribute == GX_VA_NULL) {
            count = static_cast<std::uint32_t>(entry);
            return GuestShapeError::None;
        }
        descriptors[entry] = {attribute, type};
    }
    return GuestShapeError::UnterminatedDescriptorList;
}

[[nodiscard]] GuestShapeError
read_attribute_formats(const GuestReader& reader, GuestAddress list,
                       std::array<native_render::J3dVertexFormat, MAX_LIST_ENTRIES>& formats,
                       std::uint32_t& count) {
    for (std::size_t entry = 0; entry < MAX_LIST_ENTRIES; ++entry) {
        const GuestAddress address =
            list + static_cast<std::uint32_t>(entry) * VTX_ATTR_FMT_ENTRY_BYTES;
        std::uint32_t attribute = 0;
        std::uint32_t componentCount = 0;
        std::uint32_t componentType = 0;
        std::uint8_t fraction = 0;
        if (!reader.word(address, attribute) || !reader.word(address + 4, componentCount) ||
            !reader.word(address + 8, componentType) || !reader.byte(address + 12, fraction)) {
            return GuestShapeError::UnreadableAttributeFormatList;
        }
        if (attribute == GX_VA_NULL) {
            count = static_cast<std::uint32_t>(entry);
            return GuestShapeError::None;
        }
        formats[entry] = {attribute, componentCount, componentType, fraction};
    }
    return GuestShapeError::UnterminatedAttributeFormatList;
}

} // namespace

const char* guest_shape_error_name(GuestShapeError error) noexcept {
    switch (error) {
    case GuestShapeError::None:
        return "none";
    case GuestShapeError::NoReader:
        return "no_reader";
    case GuestShapeError::NullShape:
        return "null_shape";
    case GuestShapeError::UnreadableShape:
        return "unreadable_shape";
    case GuestShapeError::NoVertexDescriptors:
        return "no_vertex_descriptors";
    case GuestShapeError::NoVertexData:
        return "no_vertex_data";
    case GuestShapeError::UnreadableVertexData:
        return "unreadable_vertex_data";
    case GuestShapeError::UnreadableSystem:
        return "unreadable_system";
    case GuestShapeError::NoAttributeFormats:
        return "no_attribute_formats";
    case GuestShapeError::UnreadableDescriptorList:
        return "unreadable_descriptor_list";
    case GuestShapeError::UnterminatedDescriptorList:
        return "unterminated_descriptor_list";
    case GuestShapeError::UnreadableAttributeFormatList:
        return "unreadable_attribute_format_list";
    case GuestShapeError::UnterminatedAttributeFormatList:
        return "unterminated_attribute_format_list";
    case GuestShapeError::UnsupportedLayout:
        return "unsupported_layout";
    case GuestShapeError::ElementOutOfRange:
        return "element_out_of_range";
    case GuestShapeError::NoDrawTable:
        return "no_draw_table";
    case GuestShapeError::UnreadableDrawTable:
        return "unreadable_draw_table";
    case GuestShapeError::NullDraw:
        return "null_draw";
    case GuestShapeError::UnreadableDraw:
        return "unreadable_draw";
    }
    return "unknown";
}

GuestShapeError read_guest_shape(const GuestMemory& memory, GuestAddress shape, GuestAddress system,
                                 GuestShape& out) noexcept {
    if (memory.read == nullptr) {
        return GuestShapeError::NoReader;
    }
    if (shape == 0) {
        return GuestShapeError::NullShape;
    }
    const GuestReader reader(memory);

    GuestShape result{};
    result.address = shape;
    std::uint8_t nbt = 0;
    GuestAddress descriptorList = 0;
    if (!reader.half(shape + SHAPE_INDEX, result.index) ||
        !reader.half(shape + SHAPE_ELEMENT_COUNT, result.elementCount) ||
        !reader.word(shape + SHAPE_FLAGS, result.flags) ||
        !reader.word(shape + SHAPE_VTX_DESC_LIST, descriptorList) ||
        !reader.byte(shape + SHAPE_NBT, nbt) ||
        !reader.word(shape + SHAPE_VERTEX_DATA, result.vertexData)) {
        return GuestShapeError::UnreadableShape;
    }
    result.normalBinormalTangent = nbt != 0;
    if (descriptorList == 0) {
        return GuestShapeError::NoVertexDescriptors;
    }
    if (result.vertexData == 0) {
        return GuestShapeError::NoVertexData;
    }

    GuestAddress attributeFormatList = 0;
    if (!reader.word(result.vertexData + VERTEX_DATA_VTX_NUM, result.vertexCount) ||
        !reader.word(result.vertexData + VERTEX_DATA_NRM_NUM, result.normalCount) ||
        !reader.word(result.vertexData + VERTEX_DATA_COL_NUM, result.colorCount) ||
        !reader.word(result.vertexData + VERTEX_DATA_ATTR_FMT_LIST, attributeFormatList)) {
        return GuestShapeError::UnreadableVertexData;
    }
    if (!reader.word(system + SYSTEM_VTX_POS, result.positions) ||
        !reader.word(system + SYSTEM_VTX_NRM, result.normals) ||
        !reader.word(system + SYSTEM_VTX_COL, result.colors)) {
        return GuestShapeError::UnreadableSystem;
    }
    if (attributeFormatList == 0) {
        return GuestShapeError::NoAttributeFormats;
    }
    for (std::uint32_t set = 0; set < native_render::kJ3dTextureCoordinateSets; ++set) {
        if (!reader.word(result.vertexData + VERTEX_DATA_TEX_COORD_ARRAY + set * 4,
                         result.textureCoordinates[set])) {
            return GuestShapeError::UnreadableVertexData;
        }
    }

    std::array<native_render::J3dVertexDescriptor, MAX_LIST_ENTRIES> descriptors{};
    std::array<native_render::J3dVertexFormat, MAX_LIST_ENTRIES> formats{};
    if (const GuestShapeError error =
            read_descriptors(reader, descriptorList, descriptors, result.descriptorCount);
        error != GuestShapeError::None) {
        return error;
    }
    if (const GuestShapeError error = read_attribute_formats(reader, attributeFormatList, formats,
                                                             result.attributeFormatCount);
        error != GuestShapeError::None) {
        return error;
    }

    if (!native_render::normalize_j3d_vertex_layout(
            std::span(descriptors).first(result.descriptorCount),
            std::span(formats).first(result.attributeFormatCount), result.normalBinormalTangent,
            result.layout)) {
        return GuestShapeError::UnsupportedLayout;
    }

    out = result;
    return GuestShapeError::None;
}

GuestShapeError read_guest_shape_element(const GuestMemory& memory, const GuestShape& shape,
                                         std::uint16_t element, GuestShapeElement& out) noexcept {
    if (memory.read == nullptr) {
        return GuestShapeError::NoReader;
    }
    if (element >= shape.elementCount) {
        return GuestShapeError::ElementOutOfRange;
    }
    const GuestReader reader(memory);

    GuestAddress drawTable = 0;
    if (!reader.word(shape.address + SHAPE_DRAWS, drawTable)) {
        return GuestShapeError::UnreadableDrawTable;
    }
    if (drawTable == 0) {
        return GuestShapeError::NoDrawTable;
    }

    GuestShapeElement result{};
    if (!reader.word(drawTable + static_cast<std::uint32_t>(element) * 4, result.draw)) {
        return GuestShapeError::UnreadableDrawTable;
    }
    if (result.draw == 0) {
        return GuestShapeError::NullDraw;
    }
    if (!reader.word(result.draw + SHAPE_DRAW_DISPLAY_LIST_SIZE, result.displayListSize) ||
        !reader.word(result.draw + SHAPE_DRAW_DISPLAY_LIST, result.displayList)) {
        return GuestShapeError::UnreadableDraw;
    }

    out = result;
    return GuestShapeError::None;
}

native_render::J3dMeshElementSource
guest_mesh_element_source(const GuestShape& shape, const GuestShapeElement& element,
                          native_render::J3dByteReader reader) noexcept {
    native_render::J3dMeshElementSource source{
        .reader = reader,
        .layout = shape.layout,
        .displayList = native_render::ByteAddress::guest(element.displayList),
        .displayListSize = element.displayListSize,
        .positions = native_render::ByteAddress::guest(shape.positions),
        .normals = native_render::ByteAddress::guest(shape.normals),
        .colors = native_render::ByteAddress::guest(shape.colors),
        .arrayByteOrder = native_render::J3dArrayByteOrder::BigEndian,
    };
    for (std::uint32_t set = 0; set < native_render::kJ3dTextureCoordinateSets; ++set) {
        source.textureCoordinates[set] =
            native_render::ByteAddress::guest(shape.textureCoordinates[set]);
    }
    return source;
}

} // namespace sb::title_adapter
