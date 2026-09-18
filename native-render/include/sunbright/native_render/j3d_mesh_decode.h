#pragma once

#include <sunbright/native_render/byte_address.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sb::native_render {

constexpr std::uint32_t kJ3dAttributeCount = 21;
constexpr std::uint32_t kJ3dTextureCoordinateSets = 4;

// Indices into `J3dVertexLayout`'s per-attribute arrays. Named here because both runtime adapters
// have to ask the same two questions of a shape -- does it carry normals, does it carry vertex
// colour -- and the answers decide which material families the shape's material can belong to.
constexpr std::uint32_t kJ3dPositionAttribute = 9;
constexpr std::uint32_t kJ3dNormalAttribute = 10;
constexpr std::uint32_t kJ3dColor0Attribute = 11;

enum class J3dAttributeType : std::uint8_t { None = 0, Direct = 1, Index8 = 2, Index16 = 3 };

struct J3dVertexLayout {
    std::uint8_t type[kJ3dAttributeCount]{};
    std::uint32_t count[kJ3dAttributeCount]{};
    std::uint32_t component[kJ3dAttributeCount]{};
    std::uint8_t fraction[kJ3dAttributeCount]{};
    std::uint32_t offset[kJ3dAttributeCount]{};
    std::uint32_t vertexSize = 0;
    bool nbt = false;
    bool valid = false;
};

using J3dReadBytes = bool (*)(ByteAddress address, std::span<std::uint8_t> destination,
                              void* context);

struct J3dByteReader {
    J3dReadBytes read = nullptr;
    void* context = nullptr;
};

enum class J3dArrayByteOrder : std::uint8_t { BigEndian, Native };

struct J3dVertexDescriptor {
    std::uint32_t attribute = 0;
    std::uint32_t type = 0;
};

struct J3dVertexFormat {
    std::uint32_t attribute = 0;
    std::uint32_t count = 0;
    std::uint32_t component = 0;
    std::uint8_t fraction = 0;
};

struct J3dMeshElementSource {
    J3dByteReader reader{};
    J3dVertexLayout layout{};
    ByteAddress displayList{};
    std::uint32_t displayListSize = 0;
    ByteAddress positions{};
    ByteAddress normals{};
    ByteAddress colors{};
    ByteAddress textureCoordinates[kJ3dTextureCoordinateSets]{};
    // Display-list commands and indices are always big-endian. This controls only indexed
    // vertex-array payloads: retail guest memory is big-endian, while the native BMD loader
    // relocates and byte-swaps those arrays for the host.
    J3dArrayByteOrder arrayByteOrder = J3dArrayByteOrder::BigEndian;
};

// The GX position/normal matrix slot a vertex selects, 0..9. The display list holds the matrix
// *register* index, which is the slot times three; converting it here rather than at every consumer
// keeps one owner of that relationship, and an index that is not a multiple of three, or names a
// slot past the tenth, is refused rather than folded onto a neighbouring matrix.
constexpr std::uint32_t kJ3dMatrixSlotCount = 10;
constexpr std::uint32_t kJ3dMatrixSlotStride = 3;

struct J3dDecodedVertex {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    std::uint32_t positionMatrixSlot = 0;
    float nx = 0.0F;
    float ny = 0.0F;
    float nz = 0.0F;
    float uv[kJ3dTextureCoordinateSets][2]{};
    std::uint32_t rgba = 0xFFFFFFFFU;
    bool operator==(const J3dDecodedVertex&) const = default;
};

enum class J3dMeshDecodeError : std::uint8_t {
    None,
    InvalidSource,
    UnsupportedLayout,
    TruncatedDisplayList,
    UnknownPrimitive,
    InvalidVertexReference,
    InvalidMatrixSlot,
    AllocationFailure,
};

struct J3dMeshDecodeResult {
    J3dMeshDecodeError error = J3dMeshDecodeError::None;
    std::uint32_t displayListOffset = 0;
    std::uint8_t opcode = 0;
};

[[nodiscard]] const char* j3d_mesh_decode_error_name(J3dMeshDecodeError error) noexcept;
[[nodiscard]] std::uint32_t j3d_attribute_vertex_bytes(std::uint32_t attribute,
                                                       const J3dVertexLayout& layout) noexcept;
[[nodiscard]] bool normalize_j3d_vertex_layout(std::span<const J3dVertexDescriptor> descriptors,
                                               std::span<const J3dVertexFormat> formats, bool nbt,
                                               J3dVertexLayout& layout) noexcept;
[[nodiscard]] bool finalize_j3d_vertex_layout(J3dVertexLayout& layout) noexcept;
[[nodiscard]] J3dMeshDecodeResult
decode_j3d_mesh_element(const J3dMeshElementSource& source,
                        std::vector<J3dDecodedVertex>& triangles) noexcept;

} // namespace sb::native_render
