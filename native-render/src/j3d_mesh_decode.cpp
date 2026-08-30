#include <sunbright/native_render/j3d_mesh_decode.h>

#include <bit>
#include <cstring>
#include <limits>
#include <new>

namespace sb::native_render {
namespace {

constexpr std::uint32_t kPositionMatrixIndex = 0;
constexpr std::uint32_t kTextureMatrixIndex0 = 1;
constexpr std::uint32_t kPosition = 9;
constexpr std::uint32_t kNormal = 10;
constexpr std::uint32_t kColor0 = 11;
constexpr std::uint32_t kColor1 = 12;
constexpr std::uint32_t kTexture0 = 13;
constexpr std::uint32_t kNbt = 25;
constexpr std::uint32_t kNormalXyz = 0;
constexpr std::uint32_t kNormalNbt = 1;
constexpr std::uint32_t kNormalNbt3 = 2;

bool read_bytes(const J3dByteReader& reader, ByteAddress address, void* output,
                std::size_t bytes) noexcept {
    return reader.read != nullptr && address.valid() && bytes != 0 &&
           reader.read(address, {static_cast<std::uint8_t*>(output), bytes}, reader.context);
}

bool read_u8(const J3dByteReader& reader, ByteAddress address, std::uint8_t& value) noexcept {
    return read_bytes(reader, address, &value, sizeof(value));
}

bool read_u16(const J3dByteReader& reader, ByteAddress address, std::uint16_t& value,
              J3dArrayByteOrder byteOrder = J3dArrayByteOrder::BigEndian) noexcept {
    std::uint8_t bytes[2]{};
    if (!read_bytes(reader, address, bytes, sizeof(bytes)))
        return false;
    if (byteOrder == J3dArrayByteOrder::Native)
        std::memcpy(&value, bytes, sizeof(value));
    else
        value = static_cast<std::uint16_t>((bytes[0] << 8U) | bytes[1]);
    return true;
}

bool read_u32(const J3dByteReader& reader, ByteAddress address, std::uint32_t& value,
              J3dArrayByteOrder byteOrder) noexcept {
    std::uint8_t bytes[4]{};
    if (!read_bytes(reader, address, bytes, sizeof(bytes)))
        return false;
    if (byteOrder == J3dArrayByteOrder::Native)
        std::memcpy(&value, bytes, sizeof(value));
    else
        value = (static_cast<std::uint32_t>(bytes[0]) << 24U) |
                (static_cast<std::uint32_t>(bytes[1]) << 16U) |
                (static_cast<std::uint32_t>(bytes[2]) << 8U) | bytes[3];
    return true;
}

bool read_f32(const J3dByteReader& reader, ByteAddress address, J3dArrayByteOrder byteOrder,
              float& value) noexcept {
    std::uint32_t bits = 0;
    if (!read_u32(reader, address, bits, byteOrder))
        return false;
    value = std::bit_cast<float>(bits);
    return true;
}

std::uint32_t component_size(std::uint32_t type) noexcept {
    switch (type) {
    case 0:
    case 1:
        return 1;
    case 2:
    case 3:
        return 2;
    case 4:
        return 4;
    default:
        return 0;
    }
}

std::uint32_t color_size(std::uint32_t type) noexcept {
    switch (type) {
    case 0:
    case 3:
        return 2;
    case 1:
    case 4:
        return 3;
    case 2:
    case 5:
        return 4;
    default:
        return 0;
    }
}

std::uint32_t component_count(std::uint32_t attribute, std::uint32_t count) noexcept {
    if (attribute == kPosition)
        return count != 0 ? 3 : 2;
    if (attribute == kNormal)
        return count == kNormalXyz ? 3 : 9;
    if (attribute >= kTexture0 && attribute <= kTexture0 + 7)
        return count != 0 ? 2 : 1;
    return 1;
}

std::uint8_t expand4(std::uint32_t value) noexcept {
    return static_cast<std::uint8_t>((value << 4U) | value);
}

std::uint8_t expand5(std::uint32_t value) noexcept {
    return static_cast<std::uint8_t>((value << 3U) | (value >> 2U));
}

std::uint8_t expand6(std::uint32_t value) noexcept {
    return static_cast<std::uint8_t>((value << 2U) | (value >> 4U));
}

std::uint32_t pack_rgba(std::uint8_t red, std::uint8_t green, std::uint8_t blue,
                        std::uint8_t alpha) noexcept {
    return static_cast<std::uint32_t>(red) << 24U | static_cast<std::uint32_t>(green) << 16U |
           static_cast<std::uint32_t>(blue) << 8U | alpha;
}

bool read_color(const J3dByteReader& reader, ByteAddress base, std::uint32_t index,
                std::uint32_t format, J3dArrayByteOrder byteOrder, std::uint32_t& rgba) noexcept {
    const std::uint32_t stride = color_size(format);
    const ByteAddress address = base.advanced(static_cast<std::uint64_t>(index) * stride);
    std::uint8_t bytes[4]{};
    if (!base.valid() || stride == 0 || !read_bytes(reader, address, bytes, stride))
        return false;
    switch (format) {
    case 0: {
        std::uint16_t value = 0;
        if (byteOrder == J3dArrayByteOrder::Native)
            std::memcpy(&value, bytes, sizeof(value));
        else
            value = static_cast<std::uint16_t>((bytes[0] << 8U) | bytes[1]);
        rgba = pack_rgba(expand5((value >> 11U) & 0x1FU), expand6((value >> 5U) & 0x3FU),
                         expand5(value & 0x1FU), 255);
        return true;
    }
    case 1:
        rgba = pack_rgba(bytes[0], bytes[1], bytes[2], 255);
        return true;
    case 2:
        rgba = pack_rgba(bytes[0], bytes[1], bytes[2], 255);
        return true;
    case 3: {
        std::uint16_t value = 0;
        if (byteOrder == J3dArrayByteOrder::Native)
            std::memcpy(&value, bytes, sizeof(value));
        else
            value = static_cast<std::uint16_t>((bytes[0] << 8U) | bytes[1]);
        rgba = pack_rgba(expand4((value >> 12U) & 0xFU), expand4((value >> 8U) & 0xFU),
                         expand4((value >> 4U) & 0xFU), expand4(value & 0xFU));
        return true;
    }
    case 4: {
        const std::uint32_t value = (static_cast<std::uint32_t>(bytes[0]) << 16U) |
                                    (static_cast<std::uint32_t>(bytes[1]) << 8U) | bytes[2];
        rgba = pack_rgba(expand6((value >> 18U) & 0x3FU), expand6((value >> 12U) & 0x3FU),
                         expand6((value >> 6U) & 0x3FU), expand6(value & 0x3FU));
        return true;
    }
    case 5:
        rgba = pack_rgba(bytes[0], bytes[1], bytes[2], bytes[3]);
        return true;
    default:
        return false;
    }
}

bool read_scalar(const J3dByteReader& reader, ByteAddress address, std::uint32_t component,
                 J3dArrayByteOrder byteOrder, float scale, float& value) noexcept {
    switch (component) {
    case 0: {
        std::uint8_t raw = 0;
        if (!read_u8(reader, address, raw))
            return false;
        value = static_cast<float>(raw) * scale;
        return true;
    }
    case 1: {
        std::uint8_t raw = 0;
        if (!read_u8(reader, address, raw))
            return false;
        value = static_cast<float>(static_cast<std::int8_t>(raw)) * scale;
        return true;
    }
    case 2: {
        std::uint16_t raw = 0;
        if (!read_u16(reader, address, raw, byteOrder))
            return false;
        value = static_cast<float>(raw) * scale;
        return true;
    }
    case 3: {
        std::uint16_t raw = 0;
        if (!read_u16(reader, address, raw, byteOrder))
            return false;
        value = static_cast<float>(static_cast<std::int16_t>(raw)) * scale;
        return true;
    }
    case 4:
        return read_f32(reader, address, byteOrder, value);
    default:
        return false;
    }
}

bool read_index(const J3dMeshElementSource& source, ByteAddress vertex, std::uint32_t attribute,
                std::uint32_t& index) noexcept {
    const auto type = static_cast<J3dAttributeType>(source.layout.type[attribute]);
    if (type == J3dAttributeType::Index8) {
        std::uint8_t value = 0;
        if (!read_u8(source.reader, vertex.advanced(source.layout.offset[attribute]), value))
            return false;
        index = value;
        return true;
    }
    if (type == J3dAttributeType::Index16) {
        std::uint16_t value = 0;
        if (!read_u16(source.reader, vertex.advanced(source.layout.offset[attribute]), value))
            return false;
        index = value;
        return true;
    }
    return false;
}

} // namespace

const char* j3d_mesh_decode_error_name(J3dMeshDecodeError error) noexcept {
    switch (error) {
    case J3dMeshDecodeError::None:
        return "none";
    case J3dMeshDecodeError::InvalidSource:
        return "invalid source";
    case J3dMeshDecodeError::UnsupportedLayout:
        return "unsupported layout";
    case J3dMeshDecodeError::TruncatedDisplayList:
        return "truncated display list";
    case J3dMeshDecodeError::UnknownPrimitive:
        return "unknown primitive";
    case J3dMeshDecodeError::InvalidVertexReference:
        return "invalid vertex reference";
    case J3dMeshDecodeError::AllocationFailure:
        return "allocation failure";
    }
    return "unknown";
}

std::uint32_t j3d_attribute_vertex_bytes(std::uint32_t attribute,
                                         const J3dVertexLayout& layout) noexcept {
    if (attribute >= kJ3dAttributeCount)
        return 0;
    if (attribute <= kTextureMatrixIndex0 + 7)
        return layout.type[attribute] == static_cast<std::uint8_t>(J3dAttributeType::None) ? 0U
                                                                                           : 1U;
    switch (static_cast<J3dAttributeType>(layout.type[attribute])) {
    case J3dAttributeType::None:
        return 0;
    case J3dAttributeType::Direct:
        if (attribute == kColor0 || attribute == kColor1)
            return color_size(layout.component[attribute]);
        return component_size(layout.component[attribute]) *
               component_count(attribute, layout.count[attribute]);
    case J3dAttributeType::Index8:
        return attribute == kNormal && layout.count[attribute] == kNormalNbt3 ? 3U : 1U;
    case J3dAttributeType::Index16:
        return attribute == kNormal && layout.count[attribute] == kNormalNbt3 ? 6U : 2U;
    }
    return 0;
}

bool normalize_j3d_vertex_layout(std::span<const J3dVertexDescriptor> descriptors,
                                 std::span<const J3dVertexFormat> formats, bool nbt,
                                 J3dVertexLayout& layout) noexcept {
    layout = {};
    for (std::uint32_t attribute = 0; attribute < kJ3dAttributeCount; ++attribute) {
        layout.component[attribute] = 4;
        layout.count[attribute] = attribute == kPosition ? 1U
                                  : attribute == kNormal ? kNormalXyz
                                                         : 1U;
        if (attribute == kColor0 || attribute == kColor1)
            layout.component[attribute] = 5;
    }
    layout.nbt = nbt;
    for (const J3dVertexDescriptor& descriptor : descriptors) {
        if (descriptor.type > static_cast<std::uint32_t>(J3dAttributeType::Index16))
            return false;
        if (descriptor.attribute == kNbt) {
            layout.type[kNormal] = static_cast<std::uint8_t>(descriptor.type);
            layout.count[kNormal] = kNormalNbt;
            layout.nbt = true;
        } else if (descriptor.attribute < kJ3dAttributeCount) {
            layout.type[descriptor.attribute] = static_cast<std::uint8_t>(descriptor.type);
        }
    }
    for (const J3dVertexFormat& format : formats) {
        const std::uint32_t slot = format.attribute == kNbt ? kNormal : format.attribute;
        if (slot >= kJ3dAttributeCount)
            continue;
        layout.count[slot] = format.count;
        layout.component[slot] = format.component;
        layout.fraction[slot] = format.fraction;
    }
    if (layout.nbt && layout.count[kNormal] != kNormalNbt3)
        layout.count[kNormal] = kNormalNbt;
    return finalize_j3d_vertex_layout(layout);
}

bool finalize_j3d_vertex_layout(J3dVertexLayout& layout) noexcept {
    std::uint32_t offset = 0;
    for (std::uint32_t attribute = 0; attribute < kJ3dAttributeCount; ++attribute) {
        if (layout.type[attribute] > static_cast<std::uint8_t>(J3dAttributeType::Index16)) {
            layout.valid = false;
            return false;
        }
        const auto type = static_cast<J3dAttributeType>(layout.type[attribute]);
        if (attribute <= kTextureMatrixIndex0 + 7 && type != J3dAttributeType::None &&
            type != J3dAttributeType::Direct) {
            layout.valid = false;
            return false;
        }
        const bool numericAttribute = attribute == kPosition || attribute == kNormal ||
                                      (attribute >= kTexture0 && attribute <= kTexture0 + 7);
        if (type != J3dAttributeType::None && numericAttribute &&
            (component_size(layout.component[attribute]) == 0 ||
             layout.fraction[attribute] >= 32)) {
            layout.valid = false;
            return false;
        }
        if (type != J3dAttributeType::None && (attribute == kColor0 || attribute == kColor1) &&
            color_size(layout.component[attribute]) == 0) {
            layout.valid = false;
            return false;
        }
        layout.offset[attribute] = offset;
        const std::uint32_t bytes = j3d_attribute_vertex_bytes(attribute, layout);
        if (bytes > std::numeric_limits<std::uint32_t>::max() - offset) {
            layout.valid = false;
            return false;
        }
        offset += bytes;
    }
    layout.vertexSize = offset;
    layout.valid =
        offset != 0 && layout.type[kPosition] != static_cast<std::uint8_t>(J3dAttributeType::None);
    return layout.valid;
}

J3dMeshDecodeResult decode_j3d_mesh_element(const J3dMeshElementSource& source,
                                            std::vector<J3dDecodedVertex>& triangles) noexcept {
    if (source.reader.read == nullptr || !source.displayList.valid() ||
        source.displayListSize == 0 || source.displayListSize > (16U << 20U)) {
        return {J3dMeshDecodeError::InvalidSource};
    }
    if (!source.layout.valid || source.layout.vertexSize == 0)
        return {J3dMeshDecodeError::UnsupportedLayout};

    const float positionScale = 1.0F / static_cast<float>(1U << source.layout.fraction[kPosition]);
    const float normalScale = 1.0F / static_cast<float>(1U << source.layout.fraction[kNormal]);
    float textureScale[kJ3dTextureCoordinateSets]{};
    for (std::uint32_t set = 0; set < kJ3dTextureCoordinateSets; ++set)
        textureScale[set] =
            1.0F / static_cast<float>(1U << source.layout.fraction[kTexture0 + set]);

    std::vector<J3dDecodedVertex> primitive;
    std::vector<J3dDecodedVertex> decodedTriangles;
    try {
        std::uint32_t offset = 0;
        while (offset < source.displayListSize) {
            std::uint8_t command = 0;
            if (!read_u8(source.reader, source.displayList.advanced(offset), command))
                return {J3dMeshDecodeError::TruncatedDisplayList, offset};
            ++offset;
            if (command == 0)
                continue;
            const std::uint8_t operation = command & 0xF8U;
            const bool primitiveCommand =
                operation == 0x80 || operation == 0x90 || operation == 0x98 || operation == 0xA0 ||
                operation == 0xA8 || operation == 0xB0 || operation == 0xB8;
            if (!primitiveCommand)
                return {J3dMeshDecodeError::UnknownPrimitive, offset - 1U, command};
            std::uint16_t vertexCount = 0;
            if (offset + 2U > source.displayListSize ||
                !read_u16(source.reader, source.displayList.advanced(offset), vertexCount)) {
                return {J3dMeshDecodeError::TruncatedDisplayList, offset, command};
            }
            offset += 2U;
            const std::uint64_t payload =
                static_cast<std::uint64_t>(vertexCount) * source.layout.vertexSize;
            if (payload > source.displayListSize - offset)
                return {J3dMeshDecodeError::TruncatedDisplayList, offset, command};

            primitive.clear();
            primitive.reserve(vertexCount);
            for (std::uint32_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
                const ByteAddress vertex = source.displayList.advanced(
                    offset + static_cast<std::uint64_t>(vertexIndex) * source.layout.vertexSize);
                J3dDecodedVertex decoded{};
                if (source.layout.type[kPositionMatrixIndex] ==
                    static_cast<std::uint8_t>(J3dAttributeType::Direct)) {
                    std::uint8_t matrixAddress = 0;
                    if (!read_u8(source.reader,
                                 vertex.advanced(source.layout.offset[kPositionMatrixIndex]),
                                 matrixAddress)) {
                        return {J3dMeshDecodeError::InvalidVertexReference, offset, command};
                    }
                    decoded.positionMatrixSlot = matrixAddress / 3U;
                }

                const auto positionType =
                    static_cast<J3dAttributeType>(source.layout.type[kPosition]);
                if (positionType == J3dAttributeType::Direct) {
                    const ByteAddress address = vertex.advanced(source.layout.offset[kPosition]);
                    const std::uint32_t components =
                        component_count(kPosition, source.layout.count[kPosition]);
                    for (std::uint32_t component = 0; component < components; ++component) {
                        if (!read_scalar(
                                source.reader,
                                address.advanced(
                                    component * component_size(source.layout.component[kPosition])),
                                source.layout.component[kPosition], J3dArrayByteOrder::BigEndian,
                                positionScale, (&decoded.x)[component])) {
                            return {J3dMeshDecodeError::InvalidVertexReference, offset, command};
                        }
                    }
                } else {
                    std::uint32_t index = 0;
                    if (!read_index(source, vertex, kPosition, index))
                        return {J3dMeshDecodeError::InvalidVertexReference, offset, command};
                    const std::uint32_t stride =
                        component_size(source.layout.component[kPosition]) * 3U;
                    for (std::uint32_t component = 0; component < 3; ++component) {
                        if (!read_scalar(
                                source.reader,
                                source.positions.advanced(
                                    static_cast<std::uint64_t>(index) * stride +
                                    component * component_size(source.layout.component[kPosition])),
                                source.layout.component[kPosition], source.arrayByteOrder,
                                positionScale, (&decoded.x)[component])) {
                            return {J3dMeshDecodeError::InvalidVertexReference, offset, command};
                        }
                    }
                }

                if (source.layout.type[kNormal] ==
                        static_cast<std::uint8_t>(J3dAttributeType::Index8) ||
                    source.layout.type[kNormal] ==
                        static_cast<std::uint8_t>(J3dAttributeType::Index16)) {
                    std::uint32_t index = 0;
                    if (!read_index(source, vertex, kNormal, index))
                        return {J3dMeshDecodeError::InvalidVertexReference, offset, command};
                    const std::uint32_t stride =
                        component_size(source.layout.component[kNormal]) * 3U;
                    for (std::uint32_t component = 0; component < 3; ++component) {
                        if (!read_scalar(
                                source.reader,
                                source.normals.advanced(
                                    static_cast<std::uint64_t>(index) * stride +
                                    component * component_size(source.layout.component[kNormal])),
                                source.layout.component[kNormal], source.arrayByteOrder,
                                normalScale, (&decoded.nx)[component])) {
                            return {J3dMeshDecodeError::InvalidVertexReference, offset, command};
                        }
                    }
                }

                const auto colorType = static_cast<J3dAttributeType>(source.layout.type[kColor0]);
                if (colorType == J3dAttributeType::Direct) {
                    if (!read_color(source.reader, vertex.advanced(source.layout.offset[kColor0]),
                                    0, source.layout.component[kColor0],
                                    J3dArrayByteOrder::BigEndian, decoded.rgba)) {
                        return {J3dMeshDecodeError::InvalidVertexReference, offset, command};
                    }
                } else if (colorType == J3dAttributeType::Index8 ||
                           colorType == J3dAttributeType::Index16) {
                    std::uint32_t index = 0;
                    if (!read_index(source, vertex, kColor0, index) ||
                        !read_color(source.reader, source.colors, index,
                                    source.layout.component[kColor0], source.arrayByteOrder,
                                    decoded.rgba)) {
                        return {J3dMeshDecodeError::InvalidVertexReference, offset, command};
                    }
                }

                for (std::uint32_t set = 0; set < kJ3dTextureCoordinateSets; ++set) {
                    const std::uint32_t attribute = kTexture0 + set;
                    const auto type = static_cast<J3dAttributeType>(source.layout.type[attribute]);
                    ByteAddress address{};
                    if (type == J3dAttributeType::Direct) {
                        address = vertex.advanced(source.layout.offset[attribute]);
                    } else if (type == J3dAttributeType::Index8 ||
                               type == J3dAttributeType::Index16) {
                        std::uint32_t index = 0;
                        if (!read_index(source, vertex, attribute, index))
                            return {J3dMeshDecodeError::InvalidVertexReference, offset, command};
                        address = source.textureCoordinates[set].advanced(
                            static_cast<std::uint64_t>(index) *
                            component_size(source.layout.component[attribute]) * 2U);
                    } else {
                        continue;
                    }
                    for (std::uint32_t component = 0; component < 2; ++component) {
                        if (!read_scalar(
                                source.reader,
                                address.advanced(
                                    component * component_size(source.layout.component[attribute])),
                                source.layout.component[attribute],
                                type == J3dAttributeType::Direct ? J3dArrayByteOrder::BigEndian
                                                                 : source.arrayByteOrder,
                                textureScale[set], decoded.uv[set][component])) {
                            return {J3dMeshDecodeError::InvalidVertexReference, offset, command};
                        }
                    }
                }
                primitive.push_back(decoded);
            }
            offset += static_cast<std::uint32_t>(payload);

            const auto emit = [&](std::size_t a, std::size_t b, std::size_t c) {
                decodedTriangles.push_back(primitive[a]);
                decodedTriangles.push_back(primitive[b]);
                decodedTriangles.push_back(primitive[c]);
            };
            if (operation == 0x90) {
                for (std::size_t vertex = 0; vertex + 2 < primitive.size(); vertex += 3)
                    emit(vertex, vertex + 1, vertex + 2);
            } else if (operation == 0x98) {
                for (std::size_t vertex = 2; vertex < primitive.size(); ++vertex) {
                    if ((vertex & 1U) != 0)
                        emit(vertex - 1, vertex - 2, vertex);
                    else
                        emit(vertex - 2, vertex - 1, vertex);
                }
            } else if (operation == 0xA0) {
                for (std::size_t vertex = 2; vertex < primitive.size(); ++vertex)
                    emit(0, vertex - 1, vertex);
            } else if (operation == 0x80) {
                for (std::size_t vertex = 0; vertex + 3 < primitive.size(); vertex += 4) {
                    emit(vertex, vertex + 1, vertex + 2);
                    emit(vertex + 2, vertex + 3, vertex);
                }
            }
        }
        triangles.insert(triangles.end(), decodedTriangles.begin(), decodedTriangles.end());
    } catch (const std::bad_alloc&) {
        return {J3dMeshDecodeError::AllocationFailure};
    }
    return {};
}

} // namespace sb::native_render
