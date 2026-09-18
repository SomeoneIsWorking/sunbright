#include <sunbright/title_adapter/guest_j3d_texgen.h>

namespace sb::title_adapter {
namespace {

// Retail J3DTexGenBlockBasic, from
// decomp/sms/include/JSystem/J3D/J3DGraphBase/Blocks/J3DTexGenBlocks.hpp.
constexpr GuestAddress TEX_GEN_BLOCK_VTABLE = 0x00;
constexpr GuestAddress TEX_GEN_BLOCK_COUNT = 0x04;

constexpr std::uint32_t TEX_GEN_BLOCK_BASIC_TYPE = 0x54474243; // 'TGBC'

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
    }
    return "unknown";
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
    state.textureCoordinateCount = result.texGenCount;
    out = result;
    return GuestTexGenError::None;
}

} // namespace sb::title_adapter
