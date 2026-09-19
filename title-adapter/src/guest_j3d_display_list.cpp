// SPDX-License-Identifier: GPL-2.0-or-later
#include <sunbright/title_adapter/guest_j3d_display_list.h>

#include <vector>

namespace sb::title_adapter {
namespace {

// GX display list opcodes. A material list is register loads only, so the primitive opcodes are
// listed to be refused rather than to be decoded.
constexpr std::uint8_t OPCODE_NOP = 0x00;
constexpr std::uint8_t OPCODE_LOAD_CP = 0x08;
constexpr std::uint8_t OPCODE_LOAD_XF = 0x10;
constexpr std::uint8_t OPCODE_INDEX_A = 0x20;
constexpr std::uint8_t OPCODE_INDEX_B = 0x28;
constexpr std::uint8_t OPCODE_INDEX_C = 0x30;
constexpr std::uint8_t OPCODE_INDEX_D = 0x38;
constexpr std::uint8_t OPCODE_CALL_DISPLAY_LIST = 0x40;
constexpr std::uint8_t OPCODE_INVALIDATE_VERTEX_CACHE = 0x48;
constexpr std::uint8_t OPCODE_LOAD_BP = 0x61;
constexpr std::uint8_t OPCODE_PRIMITIVE_FIRST = 0x80;

// BP registers, in the two blocks the hardware splits eight texmaps across.
constexpr std::uint8_t BP_TEXMAP_LOW_FIRST = 0x80;
constexpr std::uint8_t BP_TEXMAP_HIGH_FIRST = 0xA0;
constexpr std::uint8_t BP_TEXMAP_BLOCK_BYTES = 0x20;
constexpr std::uint8_t BP_SET_MODE0 = 0x00;
constexpr std::uint8_t BP_SET_MODE1 = 0x04;
constexpr std::uint8_t BP_SET_IMAGE0 = 0x08;
constexpr std::uint8_t BP_SET_IMAGE3 = 0x14;
constexpr std::uint8_t BP_SET_TLUT = 0x18;
constexpr std::uint8_t BP_LOAD_TLUT0 = 0x64;
constexpr std::uint8_t BP_LOAD_TLUT1 = 0x65;

// The minification filter, from the hardware's encoding to the one GX's own API states and every
// resource header carries. The hardware splits the choice in two -- bit 2 selects linear sampling
// within a level, bits 0-1 select what happens between levels -- where GX numbers the six
// combinations in a different order. Reading the register as though it held a GX value silently
// turns linear into near-mip-near, which the decoder then refuses as a filter with no mip chain.
// The two unused hardware encodings, 3 and 7, map to values GX does not define and are left to be
// refused rather than rounded to a neighbour.
constexpr std::uint8_t gx_min_filter(std::uint8_t hardware) noexcept {
    return static_cast<std::uint8_t>(((hardware & 0x4U) >> 2U) | ((hardware & 0x3U) << 1U));
}

// GX texture formats that index a palette. Anything else samples its own bytes.
constexpr bool indexes_palette(std::uint8_t format) noexcept {
    return format == 0x08 || format == 0x09 || format == 0x0A;
}

constexpr std::uint16_t palette_entries_for(std::uint8_t format) noexcept {
    switch (format) {
    case 0x08:
        return 16;
    case 0x09:
        return 256;
    case 0x0A:
        return 16384;
    default:
        return 0;
    }
}

// One `TX_LOADTLUT` pair, kept until a texmap names the TMEM offset it wrote.
struct PaletteLoad {
    std::uint32_t address = 0;
    std::uint16_t tmemOffset = 0;
    std::uint16_t entries = 0;
};

struct Walk {
    GuestDisplayListTextures& out;
    std::vector<PaletteLoad>& palettes;
    std::uint32_t pendingPaletteAddress = 0;
    bool hasPendingPaletteAddress = false;
    // The TLUT name each texmap selected, which is a TMEM offset plus a palette format.
    std::uint16_t tlutOffset[GUEST_MAX_TEXMAPS]{};
    std::uint8_t tlutFormat[GUEST_MAX_TEXMAPS]{};
    bool hasTlut[GUEST_MAX_TEXMAPS]{};
};

void apply_texture_register(Walk& walk, std::uint8_t reg, std::uint32_t value) {
    const std::uint8_t block =
        reg >= BP_TEXMAP_HIGH_FIRST ? BP_TEXMAP_HIGH_FIRST : BP_TEXMAP_LOW_FIRST;
    const std::uint8_t offset = static_cast<std::uint8_t>(reg - block);
    const std::size_t index =
        static_cast<std::size_t>(offset & 0x03U) + (block == BP_TEXMAP_HIGH_FIRST ? 4U : 0U);
    GuestTexmapBinding& binding = walk.out.texmap[index];
    walk.out.textureRegisterWrites += 1;

    switch (offset & ~0x03U) {
    case BP_SET_MODE0:
        binding.wrapS = static_cast<std::uint8_t>(value & 0x3U);
        binding.wrapT = static_cast<std::uint8_t>((value >> 2U) & 0x3U);
        binding.magFilter = static_cast<std::uint8_t>((value >> 4U) & 0x1U);
        binding.minFilter = gx_min_filter(static_cast<std::uint8_t>((value >> 5U) & 0x7U));
        break;
    case BP_SET_MODE1:
        // Maximum LOD in sixteenths, so a chain of N levels tops out at (N-1) * 16.
        binding.mipCount = static_cast<std::uint8_t>((((value >> 8U) & 0xFFU) / 16U) + 1U);
        break;
    case BP_SET_IMAGE0:
        binding.width = static_cast<std::uint16_t>((value & 0x3FFU) + 1U);
        binding.height = static_cast<std::uint16_t>(((value >> 10U) & 0x3FFU) + 1U);
        binding.format = static_cast<std::uint8_t>((value >> 20U) & 0xFU);
        binding.hasFormat = true;
        break;
    case BP_SET_IMAGE3:
        binding.imageAddress = (value & 0xFFFFFFU) << 5U;
        binding.hasImage = true;
        break;
    case BP_SET_TLUT:
        walk.tlutOffset[index] = static_cast<std::uint16_t>(value & 0x3FFU);
        walk.tlutFormat[index] = static_cast<std::uint8_t>((value >> 10U) & 0x3U);
        walk.hasTlut[index] = true;
        break;
    default:
        walk.out.textureRegisterWrites -= 1;
        break;
    }
}

void apply_register(Walk& walk, std::uint32_t command) {
    const auto reg = static_cast<std::uint8_t>(command >> 24U);
    const std::uint32_t value = command & 0x00FFFFFFU;
    walk.out.registerWrites += 1;

    if (reg == BP_LOAD_TLUT0) {
        walk.pendingPaletteAddress = (value & 0x1FFFFFU) << 5U;
        walk.hasPendingPaletteAddress = true;
        return;
    }
    if (reg == BP_LOAD_TLUT1) {
        if (walk.hasPendingPaletteAddress) {
            walk.palettes.push_back(
                {.address = walk.pendingPaletteAddress,
                 .tmemOffset = static_cast<std::uint16_t>(value & 0x3FFU),
                 .entries = static_cast<std::uint16_t>(((value >> 10U) & 0x3FFU) * 16U)});
            walk.hasPendingPaletteAddress = false;
        }
        return;
    }

    const bool low = reg >= BP_TEXMAP_LOW_FIRST &&
                     reg < static_cast<std::uint8_t>(BP_TEXMAP_LOW_FIRST + BP_TEXMAP_BLOCK_BYTES);
    const bool high = reg >= BP_TEXMAP_HIGH_FIRST &&
                      reg < static_cast<std::uint8_t>(BP_TEXMAP_HIGH_FIRST + BP_TEXMAP_BLOCK_BYTES);
    if (low || high) {
        apply_texture_register(walk, reg, value);
    }
}

// Matches each colour-indexed texmap to the palette load that wrote the TMEM offset it selected.
void resolve_palettes(Walk& walk) {
    for (std::size_t index = 0; index < GUEST_MAX_TEXMAPS; ++index) {
        GuestTexmapBinding& binding = walk.out.texmap[index];
        if (!binding.bound() || !indexes_palette(binding.format) || !walk.hasTlut[index]) {
            continue;
        }
        for (const PaletteLoad& load : walk.palettes) {
            if (load.tmemOffset != walk.tlutOffset[index]) {
                continue;
            }
            binding.paletteAddress = load.address;
            binding.paletteFormat = walk.tlutFormat[index];
            binding.paletteEntries =
                load.entries != 0 ? load.entries : palette_entries_for(binding.format);
        }
    }
}

} // namespace

const char* name(GuestDisplayListError error) noexcept {
    switch (error) {
    case GuestDisplayListError::None:
        return "none";
    case GuestDisplayListError::NoReader:
        return "no reader";
    case GuestDisplayListError::NullObject:
        return "null display list object";
    case GuestDisplayListError::UnreadableObject:
        return "unreadable display list object";
    case GuestDisplayListError::NullList:
        return "null display list";
    case GuestDisplayListError::EmptyList:
        return "empty display list";
    case GuestDisplayListError::ListTooLarge:
        return "display list too large";
    case GuestDisplayListError::Unreadable:
        return "unreadable display list";
    case GuestDisplayListError::Truncated:
        return "truncated display list command";
    case GuestDisplayListError::PrimitiveInMaterialList:
        return "primitive in material display list";
    case GuestDisplayListError::UnknownOpcode:
        return "unknown display list opcode";
    }
    return "unknown";
}

std::uint32_t GuestDisplayListTextures::boundCount() const noexcept {
    std::uint32_t count = 0;
    for (const GuestTexmapBinding& binding : texmap) {
        count += binding.bound() ? 1U : 0U;
    }
    return count;
}

GuestDisplayListError read_guest_display_list_textures(const GuestMemory& memory, GuestAddress list,
                                                       std::uint32_t bytes,
                                                       GuestDisplayListTextures& out) noexcept {
    out = {};
    if (memory.read == nullptr) {
        return GuestDisplayListError::NoReader;
    }
    if (list == 0) {
        return GuestDisplayListError::NullList;
    }
    if (bytes == 0) {
        return GuestDisplayListError::EmptyList;
    }
    if (bytes > GUEST_DISPLAY_LIST_MAX_BYTES) {
        return GuestDisplayListError::ListTooLarge;
    }
    out.address = list;
    out.bytes = bytes;

    std::vector<std::uint8_t> raw(bytes, 0);
    const GuestReader reader(memory);
    if (!reader.bytes(list, raw)) {
        return GuestDisplayListError::Unreadable;
    }

    std::vector<PaletteLoad> palettes;
    Walk walk{.out = out, .palettes = palettes};

    std::uint32_t at = 0;
    while (at < bytes) {
        const std::uint8_t opcode = raw[at];
        if (opcode == OPCODE_NOP) {
            // Padding to the 32-byte alignment GX requires; it ends the list rather than
            // interleaving, but counting it keeps the command total honest.
            at += 1;
            out.commands += 1;
            continue;
        }
        if (opcode >= OPCODE_PRIMITIVE_FIRST) {
            return GuestDisplayListError::PrimitiveInMaterialList;
        }

        std::uint32_t length = 0;
        switch (opcode) {
        case OPCODE_LOAD_CP:
            length = 6;
            break;
        case OPCODE_LOAD_XF: {
            if (at + 5 > bytes) {
                return GuestDisplayListError::Truncated;
            }
            const std::uint32_t words =
                static_cast<std::uint32_t>((raw[at + 1] << 8) | raw[at + 2]) + 1U;
            length = 5 + words * 4U;
            break;
        }
        case OPCODE_INDEX_A:
        case OPCODE_INDEX_B:
        case OPCODE_INDEX_C:
        case OPCODE_INDEX_D:
            length = 5;
            break;
        case OPCODE_CALL_DISPLAY_LIST:
            length = 9;
            break;
        case OPCODE_INVALIDATE_VERTEX_CACHE:
            length = 1;
            break;
        case OPCODE_LOAD_BP:
            length = 5;
            break;
        default:
            return GuestDisplayListError::UnknownOpcode;
        }

        if (at + length > bytes) {
            return GuestDisplayListError::Truncated;
        }
        if (opcode == OPCODE_LOAD_BP) {
            apply_register(walk, (static_cast<std::uint32_t>(raw[at + 1]) << 24U) |
                                     (static_cast<std::uint32_t>(raw[at + 2]) << 16U) |
                                     (static_cast<std::uint32_t>(raw[at + 3]) << 8U) |
                                     static_cast<std::uint32_t>(raw[at + 4]));
        }
        at += length;
        out.commands += 1;
    }

    resolve_palettes(walk);
    return GuestDisplayListError::None;
}

GuestDisplayListError read_guest_material_packet_textures(const GuestMemory& memory,
                                                          GuestAddress materialPacket,
                                                          GuestDisplayListTextures& out) noexcept {
    out = {};
    if (memory.read == nullptr) {
        return GuestDisplayListError::NoReader;
    }
    if (materialPacket == 0) {
        return GuestDisplayListError::NullObject;
    }
    const GuestReader reader(memory);
    GuestAddress object = 0;
    if (!reader.word(materialPacket + GUEST_DRAW_PACKET_DISPLAY_LIST, object)) {
        return GuestDisplayListError::UnreadableObject;
    }
    if (object == 0) {
        return GuestDisplayListError::NullObject;
    }
    GuestAddress data = 0;
    std::uint32_t size = 0;
    if (!reader.word(object + GUEST_DISPLAY_LIST_DATA, data) ||
        !reader.word(object + GUEST_DISPLAY_LIST_SIZE, size)) {
        return GuestDisplayListError::UnreadableObject;
    }
    return read_guest_display_list_textures(memory, data, size, out);
}

} // namespace sb::title_adapter
