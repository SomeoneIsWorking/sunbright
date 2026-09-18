#include <sunbright/title_adapter/guest_j3d_tev.h>

#include <array>
#include <tuple>

namespace sb::title_adapter {
namespace {

// Retail J3DTevBlock1/2/4/16, from
// decomp/sms/include/JSystem/J3D/J3DGraphBase/Blocks/J3DTevBlocks.hpp. Every class opens with the
// vtable pointer its virtual destructor requires and the texture numbers at 0x04; nothing after
// that is shared, which is why each layout is stated whole rather than derived from a base.
constexpr GuestAddress TEV_BLOCK_VTABLE = 0x00;

constexpr GuestAddress TEV_ORDER_BYTES = 4;
constexpr GuestAddress TEV_ORDER_TEX_COORD = 0x00;
constexpr GuestAddress TEV_ORDER_TEX_MAP = 0x01;
constexpr GuestAddress TEV_ORDER_COLOR_CHAN = 0x02;

// A J3DTevStage is the eight bytes of the stage program, and `J3dTevStageState::program` is the
// same eight bytes in the same order. The decomp adapter copies the object across whole; so does
// this, rather than naming eight fields twice.
constexpr GuestAddress TEV_STAGE_BYTES = 8;
static_assert(std::tuple_size_v<decltype(native_render::J3dTevStageState::program)> ==
              TEV_STAGE_BYTES);

// A J3DGXColorS10 is four s16 register components; a J3DGXColor is four u8.
constexpr GuestAddress TEV_COLOR_BYTES = 8;
constexpr GuestAddress KONST_COLOR_BYTES = 4;

constexpr GuestAddress TEXTURE_NUMBER_BYTES = 2;

// A block class with no stage-count field answers a constant instead: J3DTevBlock1 has exactly one
// stage and no room to say otherwise.
constexpr GuestAddress NO_FIELD = 0;

struct TevBlockLayout {
    std::uint32_t blockType;
    std::uint8_t textureBindingCount;
    std::uint8_t stageCapacity;
    GuestAddress textureNumbers;
    GuestAddress tevOrders;
    GuestAddress stageCount; // NO_FIELD when the class answers a constant.
    std::uint8_t constantStageCount;
    GuestAddress tevStages;
    GuestAddress tevColors;   // NO_FIELD when the class has none.
    GuestAddress konstColors; // NO_FIELD when the class has none.
    GuestAddress konstColorSelections;
    GuestAddress konstAlphaSelections;
};

constexpr TevBlockLayout STAGE_1{.blockType = 0x54564231, // 'TVB1'
                                 .textureBindingCount = 1,
                                 .stageCapacity = 1,
                                 .textureNumbers = 0x04,
                                 .tevOrders = 0x06,
                                 .stageCount = NO_FIELD,
                                 .constantStageCount = 1,
                                 .tevStages = 0x0a,
                                 .tevColors = NO_FIELD,
                                 .konstColors = NO_FIELD,
                                 .konstColorSelections = NO_FIELD,
                                 .konstAlphaSelections = NO_FIELD};

constexpr TevBlockLayout STAGE_2{.blockType = 0x54564232, // 'TVB2'
                                 .textureBindingCount = 2,
                                 .stageCapacity = 2,
                                 .textureNumbers = 0x04,
                                 .tevOrders = 0x08,
                                 .stageCount = 0x30,
                                 .constantStageCount = 0,
                                 .tevStages = 0x31,
                                 .tevColors = 0x10,
                                 .konstColors = 0x41,
                                 .konstColorSelections = 0x51,
                                 .konstAlphaSelections = 0x53};

constexpr TevBlockLayout STAGE_4{.blockType = 0x54564234, // 'TVB4'
                                 .textureBindingCount = 4,
                                 .stageCapacity = 4,
                                 .textureNumbers = 0x04,
                                 .tevOrders = 0x0c,
                                 .stageCount = 0x1c,
                                 .constantStageCount = 0,
                                 .tevStages = 0x1d,
                                 .tevColors = 0x3e,
                                 .konstColors = 0x5e,
                                 .konstColorSelections = 0x6e,
                                 .konstAlphaSelections = 0x72};

constexpr TevBlockLayout STAGE_16{.blockType = 0x54563136, // 'TV16'
                                  .textureBindingCount = 8,
                                  .stageCapacity = 16,
                                  .textureNumbers = 0x004,
                                  .tevOrders = 0x014,
                                  .stageCount = 0x054,
                                  .constantStageCount = 0,
                                  .tevStages = 0x055,
                                  .tevColors = 0x0d6,
                                  .konstColors = 0x0f6,
                                  .konstColorSelections = 0x106,
                                  .konstAlphaSelections = 0x116};

[[nodiscard]] GuestTevError read_stage_count(const GuestReader& reader, GuestAddress block,
                                             const TevBlockLayout& layout, std::uint8_t& count) {
    if (layout.stageCount == NO_FIELD) {
        count = layout.constantStageCount;
        return GuestTevError::None;
    }
    if (!reader.byte(block + layout.stageCount, count)) {
        return GuestTevError::UnreadableStageCount;
    }
    if (count > layout.stageCapacity) {
        return GuestTevError::StageCountPastBlockCapacity;
    }
    return GuestTevError::None;
}

[[nodiscard]] GuestTevError read_stages(const GuestReader& reader, GuestAddress block,
                                        const TevBlockLayout& layout, std::uint8_t stageCount,
                                        native_render::J3dMaterialState& state) {
    for (std::uint8_t stage = 0; stage < stageCount; ++stage) {
        native_render::J3dTevStageState& target = state.tevStages[stage];
        const GuestAddress order = block + layout.tevOrders + stage * TEV_ORDER_BYTES;
        if (!reader.byte(order + TEV_ORDER_TEX_COORD, target.textureCoordinate) ||
            !reader.byte(order + TEV_ORDER_TEX_MAP, target.textureMap) ||
            !reader.byte(order + TEV_ORDER_COLOR_CHAN, target.colorChannel)) {
            return GuestTevError::UnreadableTevOrder;
        }
        if (!reader.bytes(block + layout.tevStages + stage * TEV_STAGE_BYTES, target.program)) {
            return GuestTevError::UnreadableTevStage;
        }
    }
    return GuestTevError::None;
}

[[nodiscard]] GuestTevError read_colors(const GuestReader& reader, GuestAddress block,
                                        const TevBlockLayout& layout,
                                        native_render::J3dMaterialState& state) {
    // J3DTevBlock1 answers a null pointer for every one of these, and the decomp adapter's capture
    // fails there. Reporting that as an error rather than as an all-zero colour set is what keeps
    // the two runtimes classifying the same material the same way.
    if (layout.tevColors == NO_FIELD) {
        return GuestTevError::BlockHasNoTevColors;
    }
    for (std::size_t index = 0; index < state.tevColorsS10.size(); ++index) {
        std::array<std::uint8_t, TEV_COLOR_BYTES> raw{};
        if (!reader.bytes(block + layout.tevColors +
                              static_cast<GuestAddress>(index) * TEV_COLOR_BYTES,
                          raw)) {
            return GuestTevError::UnreadableTevColor;
        }
        for (std::size_t component = 0; component < state.tevColorsS10[index].size(); ++component) {
            state.tevColorsS10[index][component] = static_cast<std::int16_t>(
                static_cast<std::uint16_t>(raw[component * 2]) << 8U | raw[component * 2 + 1]);
        }
    }
    state.hasTevColors = true;
    for (std::size_t index = 0; index < state.konstColorRgba8.size(); ++index) {
        std::array<std::uint8_t, KONST_COLOR_BYTES> raw{};
        if (!reader.bytes(block + layout.konstColors +
                              static_cast<GuestAddress>(index) * KONST_COLOR_BYTES,
                          raw)) {
            return GuestTevError::UnreadableKonstColor;
        }
        state.konstColorRgba8[index] =
            static_cast<std::uint32_t>(raw[0]) << 24U | static_cast<std::uint32_t>(raw[1]) << 16U |
            static_cast<std::uint32_t>(raw[2]) << 8U | static_cast<std::uint32_t>(raw[3]);
    }
    // The constant selections are read for the whole capacity rather than the active stage count,
    // because that is what the decomp adapter does and the difference is visible: a stage past the
    // count still has a selection, and the two runtimes have to agree on what it is.
    for (std::uint8_t stage = 0; stage < layout.stageCapacity; ++stage) {
        if (!reader.byte(block + layout.konstColorSelections + stage,
                         state.tevStages[stage].konstColorSelection) ||
            !reader.byte(block + layout.konstAlphaSelections + stage,
                         state.tevStages[stage].konstAlphaSelection)) {
            return GuestTevError::UnreadableKonstSelection;
        }
    }
    return GuestTevError::None;
}

} // namespace

std::uint32_t guest_tev_block_type(GuestTevBlockKind kind) noexcept {
    switch (kind) {
    case GuestTevBlockKind::Stage1:
        return STAGE_1.blockType;
    case GuestTevBlockKind::Stage2:
        return STAGE_2.blockType;
    case GuestTevBlockKind::Stage4:
        return STAGE_4.blockType;
    case GuestTevBlockKind::Stage16:
        return STAGE_16.blockType;
    }
    return 0;
}

const char* name(GuestTevError error) noexcept {
    switch (error) {
    case GuestTevError::None:
        return "none";
    case GuestTevError::NoReader:
        return "no reader";
    case GuestTevError::NullBlock:
        return "null block";
    case GuestTevError::UnreadableBlock:
        return "unreadable block";
    case GuestTevError::UnknownBlockKind:
        return "unknown block kind";
    case GuestTevError::UnreadableStageCount:
        return "unreadable stage count";
    case GuestTevError::StageCountPastBlockCapacity:
        return "stage count past the block's capacity";
    case GuestTevError::UnreadableTextureNumber:
        return "unreadable texture number";
    case GuestTevError::UnreadableTevOrder:
        return "unreadable tev order";
    case GuestTevError::UnreadableTevStage:
        return "unreadable tev stage";
    case GuestTevError::BlockHasNoTevColors:
        return "block has no tev colours";
    case GuestTevError::UnreadableTevColor:
        return "unreadable tev colour";
    case GuestTevError::UnreadableKonstColor:
        return "unreadable konst colour";
    case GuestTevError::UnreadableKonstSelection:
        return "unreadable konst selection";
    }
    return "unknown";
}

GuestTevError read_guest_tev_block(const GuestMemory& memory, GuestAddress block,
                                   const GuestTevBlockVtables& vtables,
                                   native_render::J3dMaterialState& state,
                                   GuestTevBlock& out) noexcept {
    if (memory.read == nullptr) {
        return GuestTevError::NoReader;
    }
    if (block == 0) {
        return GuestTevError::NullBlock;
    }
    const GuestReader reader(memory);
    GuestAddress vtable = 0;
    if (!reader.word(block + TEV_BLOCK_VTABLE, vtable)) {
        return GuestTevError::UnreadableBlock;
    }
    GuestTevBlock result{};
    TevBlockLayout layout{};
    if (vtable == vtables.stage1) {
        result.kind = GuestTevBlockKind::Stage1;
        layout = STAGE_1;
    } else if (vtable == vtables.stage2) {
        result.kind = GuestTevBlockKind::Stage2;
        layout = STAGE_2;
    } else if (vtable == vtables.stage4) {
        result.kind = GuestTevBlockKind::Stage4;
        layout = STAGE_4;
    } else if (vtable == vtables.stage16) {
        result.kind = GuestTevBlockKind::Stage16;
        layout = STAGE_16;
    } else {
        return GuestTevError::UnknownBlockKind;
    }
    result.blockType = layout.blockType;
    result.stageCapacity = layout.stageCapacity;
    result.textureBindingCount = layout.textureBindingCount;

    std::uint8_t stageCount = 0;
    const GuestTevError countError = read_stage_count(reader, block, layout, stageCount);
    if (countError != GuestTevError::None) {
        return countError;
    }
    result.stageCount = stageCount;

    native_render::J3dMaterialState read = state;
    read.tevBlockType = layout.blockType;
    read.supportedTevBlock = true;
    read.tevStageCount = stageCount;
    read.textureBindings = {};
    read.tevStages = {};
    read.tevColorsS10 = {};
    read.konstColorRgba8 = {};
    read.hasTevColors = false;

    for (std::uint8_t binding = 0; binding < layout.textureBindingCount; ++binding) {
        if (!reader.half(block + layout.textureNumbers + binding * TEXTURE_NUMBER_BYTES,
                         read.textureBindings[binding].textureNumber)) {
            return GuestTevError::UnreadableTextureNumber;
        }
    }
    const GuestTevError stageError = read_stages(reader, block, layout, stageCount, read);
    if (stageError != GuestTevError::None) {
        return stageError;
    }
    const GuestTevError colorError = read_colors(reader, block, layout, read);
    if (colorError != GuestTevError::None) {
        return colorError;
    }

    state = read;
    out = result;
    return GuestTevError::None;
}

} // namespace sb::title_adapter
