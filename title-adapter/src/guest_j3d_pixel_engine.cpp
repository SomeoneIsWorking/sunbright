#include <sunbright/title_adapter/guest_j3d_pixel_engine.h>

#include <array>

namespace sb::title_adapter {
namespace {

// Retail J3DPEBlockFull, from decomp/sms/include/JSystem/J3D/J3DGraphBase/Blocks/J3DPEBlocks.hpp.
// The class opens with the vtable pointer its virtual destructor requires, so its members follow.
constexpr GuestAddress PE_BLOCK_VTABLE = 0x00;
constexpr GuestAddress PE_BLOCK_FOG = 0x04;
constexpr GuestAddress PE_BLOCK_ALPHA_COMP = 0x08;
constexpr GuestAddress PE_BLOCK_BLEND = 0x0c;
constexpr GuestAddress PE_BLOCK_Z_MODE = 0x10;

// Retail J3DAlphaComp: a packed id and the two reference values the table does not carry.
constexpr GuestAddress ALPHA_COMP_ID = 0x00;
constexpr GuestAddress ALPHA_COMP_REF0 = 0x02;
constexpr GuestAddress ALPHA_COMP_REF1 = 0x03;

// Retail J3DBlendInfo: four GX enumerators, one byte each.
constexpr GuestAddress BLEND_MODE = 0x00;
constexpr GuestAddress BLEND_SRC_FACTOR = 0x01;
constexpr GuestAddress BLEND_DST_FACTOR = 0x02;
constexpr GuestAddress BLEND_LOGIC_OP = 0x03;

// Retail J3DZMode: a packed id and nothing else.
constexpr GuestAddress Z_MODE_ID = 0x00;

// Retail J3DFogInfo, size 0x2C.
constexpr GuestAddress FOG_TYPE = 0x00;
constexpr GuestAddress FOG_ADJ_ENABLE = 0x01;
constexpr GuestAddress FOG_CENTER = 0x02;
constexpr GuestAddress FOG_START_Z = 0x04;
constexpr GuestAddress FOG_END_Z = 0x08;
constexpr GuestAddress FOG_NEAR_Z = 0x0c;
constexpr GuestAddress FOG_FAR_Z = 0x10;
constexpr GuestAddress FOG_COLOR = 0x14;
constexpr GuestAddress FOG_ADJ_TABLE = 0x18;

// Both lookup tables hold three bytes per id.
constexpr std::uint32_t TABLE_ENTRY_BYTES = 3;

// The id J3D writes when a material leaves that state unauthored. `load()` issues nothing for it,
// so there is no configuration to read and the decomp adapter reports no explicit policy.
constexpr std::uint16_t UNAUTHORED_ID = 0xffff;

[[nodiscard]] std::uint32_t pack_rgba8(std::array<std::uint8_t, 4> color) noexcept {
    return static_cast<std::uint32_t>(color[0]) << 24U |
           static_cast<std::uint32_t>(color[1]) << 16U |
           static_cast<std::uint32_t>(color[2]) << 8U | static_cast<std::uint32_t>(color[3]);
}

[[nodiscard]] bool read_table_entry(const GuestReader& reader, GuestAddress table, std::uint16_t id,
                                    std::array<std::uint8_t, TABLE_ENTRY_BYTES>& entry) {
    return reader.bytes(table + static_cast<GuestAddress>(id) * TABLE_ENTRY_BYTES, entry);
}

[[nodiscard]] GuestPixelEngineError read_fog(const GuestReader& reader, GuestAddress fog,
                                             native_render::J3dFogState& state) {
    native_render::J3dFogState read{};
    std::uint8_t adjustmentEnabled = 0;
    std::array<std::uint8_t, 4> color{};
    if (!reader.byte(fog + FOG_TYPE, read.type) ||
        !reader.byte(fog + FOG_ADJ_ENABLE, adjustmentEnabled) ||
        !reader.half(fog + FOG_CENTER, read.center) ||
        !reader.real(fog + FOG_START_Z, read.start) || !reader.real(fog + FOG_END_Z, read.end) ||
        !reader.real(fog + FOG_NEAR_Z, read.near) || !reader.real(fog + FOG_FAR_Z, read.far) ||
        !reader.bytes(fog + FOG_COLOR, color)) {
        return GuestPixelEngineError::UnreadableFog;
    }
    read.rangeAdjustmentEnabled = adjustmentEnabled != 0;
    read.colorRgba8 = pack_rgba8(color);
    for (std::size_t index = 0; index < read.rangeAdjustmentTable.size(); ++index) {
        const GuestAddress entry =
            fog + FOG_ADJ_TABLE + static_cast<GuestAddress>(index) * sizeof(std::uint16_t);
        if (!reader.half(entry, read.rangeAdjustmentTable[index])) {
            return GuestPixelEngineError::UnreadableFog;
        }
    }
    state = read;
    return GuestPixelEngineError::None;
}

// J3DPEBlockFull::load() issues its alpha compare only when the id is authored and its depth mode
// only when that id is authored, so a block missing either configures part of the rasteriser and
// inherits the rest. The decomp adapter treats that as no explicit policy rather than guessing at
// the inherited half, and this has to answer the same, or the same material would classify
// differently depending on which runtime read it.
[[nodiscard]] GuestPixelEngineError read_raster_policy(const GuestReader& reader,
                                                       GuestAddress block,
                                                       const GuestPixelEngineTables& tables,
                                                       native_render::J3dMaterialState& state,
                                                       GuestPixelEngineBlock& out) {
    std::uint16_t alphaCompareId = 0;
    std::uint16_t depthModeId = 0;
    if (!reader.half(block + PE_BLOCK_ALPHA_COMP + ALPHA_COMP_ID, alphaCompareId)) {
        return GuestPixelEngineError::UnreadableAlphaComp;
    }
    if (!reader.half(block + PE_BLOCK_Z_MODE + Z_MODE_ID, depthModeId)) {
        return GuestPixelEngineError::UnreadableZMode;
    }
    out.alphaCompareId = alphaCompareId;
    out.depthModeId = depthModeId;
    out.hasExplicitPixelPolicy = alphaCompareId != UNAUTHORED_ID && depthModeId != UNAUTHORED_ID;
    if (!out.hasExplicitPixelPolicy) {
        return GuestPixelEngineError::None;
    }
    if (alphaCompareId >= kGuestAlphaCompareTableEntries) {
        return GuestPixelEngineError::AlphaCompareIdOutOfRange;
    }
    if (depthModeId >= kGuestDepthModeTableEntries) {
        return GuestPixelEngineError::DepthModeIdOutOfRange;
    }
    std::array<std::uint8_t, TABLE_ENTRY_BYTES> alphaCompare{};
    if (!read_table_entry(reader, tables.alphaCompare, alphaCompareId, alphaCompare)) {
        return GuestPixelEngineError::UnreadableAlphaCompareTable;
    }
    std::array<std::uint8_t, TABLE_ENTRY_BYTES> depthMode{};
    if (!read_table_entry(reader, tables.depthMode, depthModeId, depthMode)) {
        return GuestPixelEngineError::UnreadableDepthModeTable;
    }
    std::array<std::uint8_t, 4> blend{};
    if (!reader.bytes(block + PE_BLOCK_BLEND, blend)) {
        return GuestPixelEngineError::UnreadableBlend;
    }
    if (!reader.byte(block + PE_BLOCK_ALPHA_COMP + ALPHA_COMP_REF0, state.alphaReference0) ||
        !reader.byte(block + PE_BLOCK_ALPHA_COMP + ALPHA_COMP_REF1, state.alphaReference1)) {
        return GuestPixelEngineError::UnreadableAlphaComp;
    }
    state.alphaCompare0 = alphaCompare[0];
    state.alphaOperation = alphaCompare[1];
    state.alphaCompare1 = alphaCompare[2];
    state.blendMode = blend[BLEND_MODE];
    state.blendSourceFactor = blend[BLEND_SRC_FACTOR];
    state.blendDestinationFactor = blend[BLEND_DST_FACTOR];
    state.blendLogicOperation = blend[BLEND_LOGIC_OP];
    state.depthTest = depthMode[0] != 0;
    state.depthCompare = depthMode[1];
    state.depthWrite = depthMode[2] != 0;
    return GuestPixelEngineError::None;
}

} // namespace

std::uint32_t guest_pixel_engine_block_type(GuestPixelEngineKind kind) noexcept {
    switch (kind) {
    case GuestPixelEngineKind::Opaque:
        return 0x50454f50; // 'PEOP'
    case GuestPixelEngineKind::TextureEdge:
        return 0x50454544; // 'PEED'
    case GuestPixelEngineKind::Translucent:
        return 0x5045584c; // 'PEXL'
    case GuestPixelEngineKind::Full:
        return 0x5045464c; // 'PEFL'
    }
    return 0;
}

const char* name(GuestPixelEngineError error) noexcept {
    switch (error) {
    case GuestPixelEngineError::None:
        return "none";
    case GuestPixelEngineError::NoReader:
        return "no reader";
    case GuestPixelEngineError::NullBlock:
        return "null block";
    case GuestPixelEngineError::UnreadableBlock:
        return "unreadable block";
    case GuestPixelEngineError::UnknownBlockKind:
        return "unknown block kind";
    case GuestPixelEngineError::UnreadableFog:
        return "unreadable fog";
    case GuestPixelEngineError::UnreadableAlphaComp:
        return "unreadable alpha compare";
    case GuestPixelEngineError::UnreadableBlend:
        return "unreadable blend";
    case GuestPixelEngineError::UnreadableZMode:
        return "unreadable depth mode";
    case GuestPixelEngineError::AlphaCompareIdOutOfRange:
        return "alpha compare id out of range";
    case GuestPixelEngineError::DepthModeIdOutOfRange:
        return "depth mode id out of range";
    case GuestPixelEngineError::UnreadableAlphaCompareTable:
        return "unreadable alpha compare table";
    case GuestPixelEngineError::UnreadableDepthModeTable:
        return "unreadable depth mode table";
    }
    return "unknown";
}

GuestPixelEngineError read_guest_pixel_engine_block(const GuestMemory& memory, GuestAddress block,
                                                    const GuestPixelEngineVtables& vtables,
                                                    const GuestPixelEngineTables& tables,
                                                    native_render::J3dMaterialState& state,
                                                    GuestPixelEngineBlock& out) noexcept {
    if (memory.read == nullptr) {
        return GuestPixelEngineError::NoReader;
    }
    if (block == 0) {
        return GuestPixelEngineError::NullBlock;
    }
    const GuestReader reader(memory);
    GuestAddress vtable = 0;
    if (!reader.word(block + PE_BLOCK_VTABLE, vtable)) {
        return GuestPixelEngineError::UnreadableBlock;
    }
    GuestPixelEngineBlock result{};
    if (vtable == vtables.opaque) {
        result.kind = GuestPixelEngineKind::Opaque;
    } else if (vtable == vtables.textureEdge) {
        result.kind = GuestPixelEngineKind::TextureEdge;
    } else if (vtable == vtables.translucent) {
        result.kind = GuestPixelEngineKind::Translucent;
    } else if (vtable == vtables.full) {
        result.kind = GuestPixelEngineKind::Full;
    } else {
        return GuestPixelEngineError::UnknownBlockKind;
    }
    result.blockType = guest_pixel_engine_block_type(result.kind);

    native_render::J3dMaterialState read = state;
    read.pixelEngineBlockType = result.blockType;
    read.hasExplicitPixelPolicy = false;
    read.fog = {};
    if (result.kind == GuestPixelEngineKind::Full) {
        GuestAddress fog = 0;
        if (!reader.word(block + PE_BLOCK_FOG, fog)) {
            return GuestPixelEngineError::UnreadableFog;
        }
        result.hasFog = fog != 0;
        if (result.hasFog) {
            const GuestPixelEngineError error = read_fog(reader, fog, read.fog);
            if (error != GuestPixelEngineError::None) {
                return error;
            }
        }
        const GuestPixelEngineError error = read_raster_policy(reader, block, tables, read, result);
        if (error != GuestPixelEngineError::None) {
            return error;
        }
        read.hasExplicitPixelPolicy = result.hasExplicitPixelPolicy;
    }
    state = read;
    out = result;
    return GuestPixelEngineError::None;
}

} // namespace sb::title_adapter
