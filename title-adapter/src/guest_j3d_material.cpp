#include <sunbright/title_adapter/guest_j3d_material.h>

namespace sb::title_adapter {
namespace {

// Retail J3DMaterial, from decomp/sms/include/JSystem/J3D/J3DGraphBase/J3DMaterial.hpp.
constexpr GuestAddress MATERIAL_COLOR_BLOCK = 0x20;
constexpr GuestAddress MATERIAL_TEX_GEN_BLOCK = 0x24;
constexpr GuestAddress MATERIAL_TEV_BLOCK = 0x28;
constexpr GuestAddress MATERIAL_PE_BLOCK = 0x30;

} // namespace

const char* name(GuestMaterialError error) noexcept {
    switch (error) {
    case GuestMaterialError::None:
        return "none";
    case GuestMaterialError::NoReader:
        return "no reader";
    case GuestMaterialError::NullMaterial:
        return "null material";
    case GuestMaterialError::UnreadableMaterial:
        return "unreadable material";
    case GuestMaterialError::ColorBlock:
        return "colour block";
    case GuestMaterialError::TexGenBlock:
        return "texture generation block";
    case GuestMaterialError::TevBlock:
        return "tev block";
    case GuestMaterialError::PixelEngineBlock:
        return "pixel engine block";
    }
    return "unknown";
}

GuestMaterialError read_guest_material(const GuestMemory& memory, GuestAddress material,
                                       const GuestMaterialVtables& vtables,
                                       const GuestPixelEngineTables& tables, bool hasVertexColor,
                                       bool hasNormal, native_render::J3dMaterialState& state,
                                       GuestMaterial& out) noexcept {
    if (memory.read == nullptr) {
        return GuestMaterialError::NoReader;
    }
    if (material == 0) {
        return GuestMaterialError::NullMaterial;
    }
    const GuestReader reader(memory);
    GuestMaterial result{};
    if (!reader.word(material + MATERIAL_COLOR_BLOCK, result.colorBlock) ||
        !reader.word(material + MATERIAL_TEX_GEN_BLOCK, result.texGenBlock) ||
        !reader.word(material + MATERIAL_TEV_BLOCK, result.tevBlock) ||
        !reader.word(material + MATERIAL_PE_BLOCK, result.pixelEngineBlock)) {
        return GuestMaterialError::UnreadableMaterial;
    }

    // Built up in one state and published only once every block has been read, so a material whose
    // TEV block is unreadable leaves the caller's state as it was rather than half-replaced.
    native_render::J3dMaterialState read{};
    read.hasVertexColor = hasVertexColor;
    read.hasNormal = hasNormal;

    result.colorError =
        read_guest_color_block(memory, result.colorBlock, vtables.color, read, result.color);
    if (result.colorError != GuestColorError::None) {
        out = result;
        return GuestMaterialError::ColorBlock;
    }
    result.texGenError =
        read_guest_tex_gen_block(memory, result.texGenBlock, vtables.texGen, read, result.texGen);
    if (result.texGenError != GuestTexGenError::None) {
        out = result;
        return GuestMaterialError::TexGenBlock;
    }
    result.tevError = read_guest_tev_block(memory, result.tevBlock, vtables.tev, read, result.tev);
    if (result.tevError != GuestTevError::None) {
        out = result;
        return GuestMaterialError::TevBlock;
    }
    result.pixelEngineError = read_guest_pixel_engine_block(
        memory, result.pixelEngineBlock, vtables.pixelEngine, tables, read, result.pixelEngine);
    if (result.pixelEngineError != GuestPixelEngineError::None) {
        out = result;
        return GuestMaterialError::PixelEngineBlock;
    }

    state = read;
    out = result;
    return GuestMaterialError::None;
}

} // namespace sb::title_adapter
