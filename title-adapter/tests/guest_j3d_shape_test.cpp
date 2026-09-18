// Drives the shipping guest-layout reader against a synthetic guest image. Every field this test
// asserts is one the retail layout places at a fixed offset, so a wrong offset shows up here rather
// than as geometry nobody can trace back to a struct.

#include <sunbright/title_adapter/guest_j3d_shape.h>

#include "guest_image.h"

#include <array>
#include <cassert>
#include <string_view>

namespace {

using sb::title_adapter::GuestAddress;
using sb::title_adapter::GuestMemory;
using sb::title_adapter::GuestShape;
using sb::title_adapter::GuestShapeElement;
using sb::title_adapter::GuestShapeError;
using sb::title_adapter::test::Image;
using sb::title_adapter::test::read_image;

constexpr GuestAddress SHAPE = 0x80000100;
constexpr GuestAddress VERTEX_DATA = 0x80000200;
constexpr GuestAddress SYSTEM = 0x80000300;
constexpr GuestAddress DESCRIPTORS = 0x80000500;
constexpr GuestAddress FORMATS = 0x80000600;
constexpr GuestAddress DRAW_TABLE = 0x80000700;
constexpr GuestAddress DRAW_ZERO = 0x80000740;
constexpr GuestAddress DRAW_ONE = 0x80000760;
constexpr GuestAddress DISPLAY_LIST = 0x80000800;

constexpr std::uint32_t GX_VA_POS = 9;
constexpr std::uint32_t GX_VA_TEX0 = 13;
constexpr std::uint32_t GX_VA_NULL = 0xff;
constexpr std::uint32_t GX_INDEX16 = 3;
constexpr std::uint32_t GX_POS_XYZ = 1;
constexpr std::uint32_t GX_TEX_ST = 1;
constexpr std::uint32_t GX_F32 = 4;

// A shape whose fields are all distinct values, so a reader that fetched the neighbouring word
// would produce a wrong answer rather than an accidentally right one.
void build(Image& image) {
    image.half(SHAPE + 0x04, 0x1234);      // mIndex
    image.half(SHAPE + 0x06, 2);           // mElementCount
    image.word(SHAPE + 0x08, 0x00000011);  // mFlags: Visible | EnableLod
    image.word(SHAPE + 0x2c, DESCRIPTORS); // mVtxDescList
    image.byte(SHAPE + 0x30, 1);           // unk30 (NBT)
    image.word(SHAPE + 0x38, DRAW_TABLE);  // mDraws
    image.word(SHAPE + 0x44, VERTEX_DATA); // mVertexData

    image.word(VERTEX_DATA + 0x00, 64);         // mVtxNum
    image.word(VERTEX_DATA + 0x04, 32);         // mNrmNum
    image.word(VERTEX_DATA + 0x08, 16);         // mColNum
    image.word(VERTEX_DATA + 0x0c, FORMATS);    // mVtxAttrFmtList
    image.word(VERTEX_DATA + 0x10, 0xdeadbe00); // mVtxPosArray -- deliberately NOT what is read
    image.word(VERTEX_DATA + 0x24, 0x80000a00); // mVtxTexCoordArray[0]
    image.word(VERTEX_DATA + 0x28, 0x80000a40); // mVtxTexCoordArray[1]

    image.word(SYSTEM + 0x10c, 0x80000900); // j3dSys.mVtxPos
    image.word(SYSTEM + 0x110, 0x80000940); // j3dSys.mVtxNrm
    image.word(SYSTEM + 0x114, 0x80000980); // j3dSys.mVtxCol

    image.word(DESCRIPTORS + 0x00, GX_VA_POS);
    image.word(DESCRIPTORS + 0x04, GX_INDEX16);
    image.word(DESCRIPTORS + 0x08, GX_VA_TEX0);
    image.word(DESCRIPTORS + 0x0c, GX_INDEX16);
    image.word(DESCRIPTORS + 0x10, GX_VA_NULL);

    image.word(FORMATS + 0x00, GX_VA_POS);
    image.word(FORMATS + 0x04, GX_POS_XYZ);
    image.word(FORMATS + 0x08, GX_F32);
    image.byte(FORMATS + 0x0c, 0);
    image.word(FORMATS + 0x10, GX_VA_TEX0);
    image.word(FORMATS + 0x14, GX_TEX_ST);
    image.word(FORMATS + 0x18, GX_F32);
    image.byte(FORMATS + 0x1c, 0);
    image.word(FORMATS + 0x20, GX_VA_NULL);

    image.word(DRAW_TABLE + 0x00, DRAW_ZERO);
    image.word(DRAW_TABLE + 0x04, DRAW_ONE);
    image.word(DRAW_ZERO + 0x04, 0x60);         // mDisplayListSize
    image.word(DRAW_ZERO + 0x08, DISPLAY_LIST); // mDisplayList
    image.word(DRAW_ONE + 0x04, 0x20);
    image.word(DRAW_ONE + 0x08, DISPLAY_LIST + 0x60);
}

void reads_every_field() {
    Image image;
    build(image);
    const GuestMemory memory{read_image, &image};

    GuestShape shape{};
    assert(read_guest_shape(memory, SHAPE, SYSTEM, shape) == GuestShapeError::None);
    assert(shape.address == SHAPE);
    assert(shape.index == 0x1234);
    assert(shape.elementCount == 2);
    assert(shape.flags == 0x00000011);
    assert(shape.normalBinormalTangent);
    assert(shape.vertexData == VERTEX_DATA);
    assert(shape.vertexCount == 64);
    assert(shape.normalCount == 32);
    assert(shape.colorCount == 16);
    // The current arrays come from j3dSys. 0xdeadbe00 sits in J3DVertexData's own position pointer
    // precisely so that reading the wrong one is visible instead of plausible.
    assert(shape.positions == 0x80000900);
    assert(shape.normals == 0x80000940);
    assert(shape.colors == 0x80000980);
    assert(shape.textureCoordinates[0] == 0x80000a00);
    assert(shape.textureCoordinates[1] == 0x80000a40);
    assert(shape.descriptorCount == 2);
    assert(shape.attributeFormatCount == 2);
    assert(shape.layout.valid);
    assert(shape.layout.nbt);
    assert(shape.layout.type[GX_VA_POS] ==
           static_cast<std::uint8_t>(sb::native_render::J3dAttributeType::Index16));
    assert(shape.layout.type[GX_VA_TEX0] ==
           static_cast<std::uint8_t>(sb::native_render::J3dAttributeType::Index16));

    GuestShapeElement element{};
    assert(read_guest_shape_element(memory, shape, 0, element) == GuestShapeError::None);
    assert(element.draw == DRAW_ZERO);
    assert(element.displayList == DISPLAY_LIST);
    assert(element.displayListSize == 0x60);
    assert(read_guest_shape_element(memory, shape, 1, element) == GuestShapeError::None);
    assert(element.draw == DRAW_ONE);
    assert(element.displayList == DISPLAY_LIST + 0x60);
    assert(element.displayListSize == 0x20);

    const sb::native_render::J3dMeshElementSource source =
        guest_mesh_element_source(shape, element, {nullptr, nullptr});
    std::uint64_t address = 0;
    assert(source.displayList.guest_value(address) && address == DISPLAY_LIST + 0x60);
    assert(source.displayListSize == 0x20);
    assert(source.positions.guest_value(address) && address == 0x80000900);
    assert(source.textureCoordinates[1].guest_value(address) && address == 0x80000a40);
    assert(source.arrayByteOrder == sb::native_render::J3dArrayByteOrder::BigEndian);
}

void refuses_what_it_cannot_read() {
    Image image;
    build(image);
    const GuestMemory memory{read_image, &image};
    GuestShape shape{};

    assert(read_guest_shape({nullptr, nullptr}, SHAPE, SYSTEM, shape) == GuestShapeError::NoReader);
    assert(read_guest_shape(memory, 0, SYSTEM, shape) == GuestShapeError::NullShape);
    // Below the image entirely: the reader answers false, and that must not become an empty shape.
    assert(read_guest_shape(memory, 0x00001000, SYSTEM, shape) == GuestShapeError::UnreadableShape);
    assert(read_guest_shape(memory, SHAPE, 0x00001000, shape) == GuestShapeError::UnreadableSystem);

    Image noDescriptors = image;
    noDescriptors.word(SHAPE + 0x2c, 0);
    const GuestMemory noDescriptorsMemory{read_image, &noDescriptors};
    assert(read_guest_shape(noDescriptorsMemory, SHAPE, SYSTEM, shape) ==
           GuestShapeError::NoVertexDescriptors);

    Image noVertexData = image;
    noVertexData.word(SHAPE + 0x44, 0);
    const GuestMemory noVertexDataMemory{read_image, &noVertexData};
    assert(read_guest_shape(noVertexDataMemory, SHAPE, SYSTEM, shape) ==
           GuestShapeError::NoVertexData);

    Image noFormats = image;
    noFormats.word(VERTEX_DATA + 0x0c, 0);
    const GuestMemory noFormatsMemory{read_image, &noFormats};
    assert(read_guest_shape(noFormatsMemory, SHAPE, SYSTEM, shape) ==
           GuestShapeError::NoAttributeFormats);

    // A descriptor list with no GX_VA_NULL is not a short list, it is not a list. Overwriting the
    // terminator with a real attribute is the exact corruption that would otherwise be read as a
    // 64-entry vertex format and produce a mesh with the wrong stride.
    Image unterminated = image;
    for (std::uint32_t entry = 0; entry < 80; ++entry) {
        unterminated.word(DESCRIPTORS + entry * 8, GX_VA_POS);
        unterminated.word(DESCRIPTORS + entry * 8 + 4, GX_INDEX16);
    }
    const GuestMemory unterminatedMemory{read_image, &unterminated};
    assert(read_guest_shape(unterminatedMemory, SHAPE, SYSTEM, shape) ==
           GuestShapeError::UnterminatedDescriptorList);
}

void refuses_elements_it_has_not_got() {
    Image image;
    build(image);
    const GuestMemory memory{read_image, &image};
    GuestShape shape{};
    assert(read_guest_shape(memory, SHAPE, SYSTEM, shape) == GuestShapeError::None);

    GuestShapeElement element{};
    assert(read_guest_shape_element(memory, shape, 2, element) ==
           GuestShapeError::ElementOutOfRange);
    assert(read_guest_shape_element({nullptr, nullptr}, shape, 0, element) ==
           GuestShapeError::NoReader);

    Image nullDraw = image;
    nullDraw.word(DRAW_TABLE + 0x00, 0);
    const GuestMemory nullDrawMemory{read_image, &nullDraw};
    assert(read_guest_shape_element(nullDrawMemory, shape, 0, element) ==
           GuestShapeError::NullDraw);

    Image noTable = image;
    noTable.word(SHAPE + 0x38, 0);
    const GuestMemory noTableMemory{read_image, &noTable};
    assert(read_guest_shape_element(noTableMemory, shape, 0, element) ==
           GuestShapeError::NoDrawTable);
}

void names_every_error() {
    // A report that prints an error number is a report someone has to look up. Every enumerator has
    // a name, and the default must not be reachable for a real one.
    constexpr std::array errors = {
        GuestShapeError::None,
        GuestShapeError::NoReader,
        GuestShapeError::NullShape,
        GuestShapeError::UnreadableShape,
        GuestShapeError::NoVertexDescriptors,
        GuestShapeError::NoVertexData,
        GuestShapeError::UnreadableVertexData,
        GuestShapeError::UnreadableSystem,
        GuestShapeError::NoAttributeFormats,
        GuestShapeError::UnreadableDescriptorList,
        GuestShapeError::UnterminatedDescriptorList,
        GuestShapeError::UnreadableAttributeFormatList,
        GuestShapeError::UnterminatedAttributeFormatList,
        GuestShapeError::UnsupportedLayout,
        GuestShapeError::ElementOutOfRange,
        GuestShapeError::NoDrawTable,
        GuestShapeError::UnreadableDrawTable,
        GuestShapeError::NullDraw,
        GuestShapeError::UnreadableDraw,
    };
    std::vector<std::string_view> names;
    for (const GuestShapeError error : errors) {
        const std::string_view name = guest_shape_error_name(error);
        assert(name != "unknown");
        for (const std::string_view seen : names) {
            assert(seen != name);
        }
        names.push_back(name);
    }
}

} // namespace

int main() {
    reads_every_field();
    refuses_what_it_cannot_read();
    refuses_elements_it_has_not_got();
    names_every_error();
    return 0;
}
