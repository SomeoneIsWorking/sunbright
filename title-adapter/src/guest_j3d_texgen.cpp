#include <sunbright/title_adapter/guest_j3d_texgen.h>

namespace sb::title_adapter {
namespace {

// Retail J3DTexGenBlockBasic, from
// decomp/sms/include/JSystem/J3D/J3DGraphBase/Blocks/J3DTexGenBlocks.hpp.
constexpr GuestAddress TEX_GEN_BLOCK_VTABLE = 0x00;
constexpr GuestAddress TEX_GEN_BLOCK_COUNT = 0x04;
constexpr GuestAddress TEX_GEN_BLOCK_TEX_COORD = 0x08;
constexpr GuestAddress TEX_GEN_BLOCK_TEX_MATRIX = 0x28;

// J3DTexCoordInfo is three bytes aligned to four, so the array strides by four.
constexpr GuestAddress TEX_COORD_STRIDE = 0x04;
constexpr GuestAddress TEX_COORD_GEN_TYPE = 0x00;
constexpr GuestAddress TEX_COORD_GEN_SRC = 0x01;
constexpr GuestAddress TEX_COORD_GEN_MATRIX = 0x02;

constexpr GuestAddress TEX_MATRIX_POINTER_STRIDE = 0x04;
constexpr GuestAddress TEX_MATRIX_PROJECTION = 0x00;
constexpr GuestAddress TEX_MATRIX_INFO = 0x01;
// J3DTexMtx::mTotalMtx, past the 0x64-byte J3DTexMtxInfo the class derives from.
constexpr GuestAddress TEX_MATRIX_TOTAL = 0x64;
constexpr std::uint32_t TEX_MATRIX_TOTAL_ELEMENTS = 12;

constexpr std::uint32_t TEX_GEN_BLOCK_BASIC_TYPE = 0x54474243; // 'TGBC'

// Reads one J3DTexMtx the block points at. A null slot is not a failure: most materials transform
// none of their coordinates, and the block holds eight pointers whatever it uses.
GuestTexGenError read_tex_matrix(const GuestReader& reader, GuestAddress block, std::uint32_t slot,
                                 GuestTexMatrix& out) {
    GuestAddress matrix = 0;
    if (!reader.word(block + TEX_GEN_BLOCK_TEX_MATRIX + (slot * TEX_MATRIX_POINTER_STRIDE),
                     matrix)) {
        return GuestTexGenError::UnreadableTexMatrixPointer;
    }
    if (matrix == 0) {
        return GuestTexGenError::None;
    }
    GuestTexMatrix result{};
    if (!reader.byte(matrix + TEX_MATRIX_PROJECTION, result.projection) ||
        !reader.byte(matrix + TEX_MATRIX_INFO, result.info)) {
        return GuestTexGenError::UnreadableTexMatrix;
    }
    for (std::uint32_t element = 0; element < TEX_MATRIX_TOTAL_ELEMENTS; ++element) {
        if (!reader.real(matrix + TEX_MATRIX_TOTAL + (element * 4U), result.total[element])) {
            return GuestTexGenError::UnreadableTexMatrix;
        }
    }
    result.present = true;
    out = result;
    return GuestTexGenError::None;
}

} // namespace

const char* name(GuestTexGenError error) noexcept {
    switch (error) {
    case GuestTexGenError::None:
        return "none";
    case GuestTexGenError::NoReader:
        return "no reader";
    case GuestTexGenError::NullBlock:
        return "null block";
    case GuestTexGenError::UnreadableBlock:
        return "unreadable block";
    case GuestTexGenError::UnreadableTexGenCount:
        return "unreadable texture coordinate count";
    case GuestTexGenError::TexGenCountOutOfRange:
        return "texture coordinate count out of range";
    case GuestTexGenError::UnreadableTexCoord:
        return "unreadable texture coordinate";
    case GuestTexGenError::UnreadableTexMatrixPointer:
        return "unreadable texture matrix pointer";
    case GuestTexGenError::UnreadableTexMatrix:
        return "unreadable texture matrix";
    }
    return "unknown";
}

std::uint32_t guest_tex_gen_matrix_slot(std::uint8_t texGenMatrix) noexcept {
    if (texGenMatrix < kGuestTexGenMatrix0 || texGenMatrix >= kGuestTexGenMatrixIdentity) {
        return kGuestMaxTexMatrixCount;
    }
    const std::uint32_t offset = static_cast<std::uint32_t>(texGenMatrix - kGuestTexGenMatrix0);
    if (offset % kGuestTexGenMatrixStride != 0) {
        return kGuestMaxTexMatrixCount;
    }
    const std::uint32_t slot = offset / kGuestTexGenMatrixStride;
    return slot < kGuestMaxTexMatrixCount ? slot : kGuestMaxTexMatrixCount;
}

native_render::J3dTexCoordGeneration
build_guest_tex_coord_generation(const GuestTexGenBlock& block) noexcept {
    native_render::J3dTexCoordGeneration generation{};
    if (!block.recognised || block.texGenCount > kGuestMaxTexGenCount) {
        return generation;
    }
    generation.count = block.texGenCount;
    for (std::uint32_t coordinate = 0; coordinate < block.texGenCount; ++coordinate) {
        const GuestTexCoordDefinition& definition = block.coordinates[coordinate];
        native_render::J3dTexCoordGenerator& generator = generation.generators[coordinate];
        generator.type = static_cast<native_render::J3dTexGenType>(definition.texGenType);
        generator.source = definition.texGenSrc;
        const std::uint32_t slot = guest_tex_gen_matrix_slot(definition.texGenMatrix);
        if (slot >= kGuestMaxTexMatrixCount || !block.matrices[slot].present) {
            continue;
        }
        generator.hasMatrix = true;
        generator.matrix = block.matrices[slot].total;
    }
    return generation;
}

GuestTexGenError read_guest_tex_gen_block(const GuestMemory& memory, GuestAddress block,
                                          const GuestTexGenBlockVtables& vtables,
                                          native_render::J3dMaterialState& state,
                                          GuestTexGenBlock& out) noexcept {
    if (memory.read == nullptr) {
        return GuestTexGenError::NoReader;
    }
    if (block == 0) {
        return GuestTexGenError::NullBlock;
    }
    const GuestReader reader(memory);
    GuestAddress vtable = 0;
    if (!reader.word(block + TEX_GEN_BLOCK_VTABLE, vtable)) {
        return GuestTexGenError::UnreadableBlock;
    }
    GuestTexGenBlock result{};
    if (vtable != vtables.basic) {
        state.textureCoordinateCount = kGuestUnsupportedTexGenCount;
        out = result;
        return GuestTexGenError::None;
    }
    result.recognised = true;
    result.blockType = TEX_GEN_BLOCK_BASIC_TYPE;
    if (!reader.word(block + TEX_GEN_BLOCK_COUNT, result.texGenCount)) {
        return GuestTexGenError::UnreadableTexGenCount;
    }
    if (result.texGenCount > kGuestMaxTexGenCount) {
        return GuestTexGenError::TexGenCountOutOfRange;
    }
    // Every coordinate the block generates is read, and only those: the remaining entries of the
    // array hold whatever the block was initialized with, and reporting them would invite a
    // consumer to transform a coordinate the material does not generate.
    for (std::uint32_t coordinate = 0; coordinate < result.texGenCount; ++coordinate) {
        const GuestAddress entry =
            block + TEX_GEN_BLOCK_TEX_COORD + (coordinate * TEX_COORD_STRIDE);
        GuestTexCoordDefinition& definition = result.coordinates[coordinate];
        if (!reader.byte(entry + TEX_COORD_GEN_TYPE, definition.texGenType) ||
            !reader.byte(entry + TEX_COORD_GEN_SRC, definition.texGenSrc) ||
            !reader.byte(entry + TEX_COORD_GEN_MATRIX, definition.texGenMatrix)) {
            return GuestTexGenError::UnreadableTexCoord;
        }
    }
    for (std::uint32_t slot = 0; slot < kGuestMaxTexMatrixCount; ++slot) {
        const GuestTexGenError error = read_tex_matrix(reader, block, slot, result.matrices[slot]);
        if (error != GuestTexGenError::None) {
            return error;
        }
    }
    state.textureCoordinateCount = result.texGenCount;
    out = result;
    return GuestTexGenError::None;
}

} // namespace sb::title_adapter
