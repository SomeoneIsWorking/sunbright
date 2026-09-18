#pragma once

#include <sunbright/native_render/j3d_mesh_decode.h>

#include <cstdint>
#include <span>

namespace sb::title_adapter {

// Reads GMSE01's own J3D objects out of guest memory, in the layout the retail PowerPC build uses.
//
// Under the dynarec there is no decomp object to look at: guest code owns these structures, they
// are 32-bit and big-endian, and the only handle on them is an address. `native-render` already
// decodes a mesh from an address plus a byte reader (`J3dMeshElementSource`), so what is missing is
// exactly this -- the step from "a J3DShape lives at 0x…" to the fields that source needs.
//
// Field offsets come from `decomp/sms`, which is a matching decompilation and therefore states the
// retail layout directly in its headers (`JSystem/J3D/J3DGraphBase/J3DShape.hpp`,
// `J3DVertex.hpp`). They are named against those headers here so a layout change shows up as a
// compile-time mismatch in one place rather than as wrong geometry.
//
// Nothing here depends on gcnport or Dolphin. The caller supplies the reader, so the same code
// serves a live runtime, a memory dump and a test fixture, and the tests exercise the shipping
// implementation rather than a second copy of it.

using GuestAddress = std::uint32_t;

// GMSE01's `j3dSys`. J3DShape::loadVtxArray at 0x802e0320 forms it in two instructions
// (`lis r4, -0x7fc0; addi r31, r4, 0x45dc`) and then reads 0x10c/0x110/0x114 out of it for
// GX_VA_POS, GX_VA_NRM and GX_VA_CLR0 -- the same three arrays this adapter needs, from the same
// place the title itself takes them. That disassembly is also what independently confirms the
// J3DSys field offsets the decomp header states, and J3DShape's own NBT flag at 0x30, which
// loadVtxArray tests with `lbz r0, 0x30(r30)` before loading the normal array.
constexpr GuestAddress GMSE01_J3D_SYS = 0x804045dc;

// Reads `destination.size()` bytes of guest memory at `address`. Must answer false when the range
// is not wholly readable: a reader that zero-fills instead turns "this address is not mapped" into
// a shape with no vertices, which is a legitimate-looking answer to a question that failed.
using GuestReadBytes = bool (*)(GuestAddress address, std::span<std::uint8_t> destination,
                                void* context);

struct GuestMemory {
    GuestReadBytes read = nullptr;
    void* context = nullptr;
};

enum class GuestShapeError : std::uint8_t {
    None,
    NoReader,
    NullShape,
    UnreadableShape,
    NoVertexDescriptors,
    NoVertexData,
    UnreadableVertexData,
    UnreadableSystem,
    NoAttributeFormats,
    UnreadableDescriptorList,
    UnterminatedDescriptorList,
    UnreadableAttributeFormatList,
    UnterminatedAttributeFormatList,
    UnsupportedLayout,
    ElementOutOfRange,
    NoDrawTable,
    UnreadableDrawTable,
    NullDraw,
    UnreadableDraw,
};

[[nodiscard]] const char* guest_shape_error_name(GuestShapeError error) noexcept;

// One J3DShape, read. `layout` is already normalized through `native-render`'s own
// `normalize_j3d_vertex_layout`, so this type never carries a second opinion about what a vertex
// looks like.
//
// Positions, normals and colours are the *current* arrays from `j3dSys`, not `J3DVertexData`'s own
// pointers. The two differ: J3DVertexBuffer swaps the current arrays per frame out of the vertex
// data's double-buffered pairs, so reading the vertex data directly would decode last frame's
// geometry (or the wrong buffer) whenever a shape is deformed on the CPU. Texture coordinates have
// no such current-array indirection and come from the vertex data.
struct GuestShape {
    GuestAddress address = 0;
    std::uint16_t index = 0;
    std::uint16_t elementCount = 0;
    std::uint32_t flags = 0;
    bool normalBinormalTangent = false;

    GuestAddress vertexData = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t normalCount = 0;
    std::uint32_t colorCount = 0;

    // From j3dSys, as loadVtxArray takes them.
    GuestAddress positions = 0;
    GuestAddress normals = 0;
    GuestAddress colors = 0;
    GuestAddress textureCoordinates[native_render::kJ3dTextureCoordinateSets]{};

    std::uint32_t descriptorCount = 0;
    std::uint32_t attributeFormatCount = 0;
    native_render::J3dVertexLayout layout{};
};

// One of the shape's matrix groups: the display list that draws it.
struct GuestShapeElement {
    GuestAddress draw = 0;
    GuestAddress displayList = 0;
    std::uint32_t displayListSize = 0;
};

// `system` is the guest address of `j3dSys` -- GMSE01_J3D_SYS for the retail title. It is a
// parameter rather than a baked constant so the tests can place a system object wherever they like
// and still drive this exact function.
[[nodiscard]] GuestShapeError read_guest_shape(const GuestMemory& memory, GuestAddress shape,
                                               GuestAddress system, GuestShape& out) noexcept;

[[nodiscard]] GuestShapeError read_guest_shape_element(const GuestMemory& memory,
                                                       const GuestShape& shape,
                                                       std::uint16_t element,
                                                       GuestShapeElement& out) noexcept;

// Builds the source `native_render::decode_j3d_mesh_element` consumes. Every address is a guest
// address and every indexed array stays big-endian, which is what retail guest memory holds.
[[nodiscard]] native_render::J3dMeshElementSource
guest_mesh_element_source(const GuestShape& shape, const GuestShapeElement& element,
                          native_render::J3dByteReader reader) noexcept;

} // namespace sb::title_adapter
