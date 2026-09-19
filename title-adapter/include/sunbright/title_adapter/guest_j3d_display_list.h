#pragma once

#include <sunbright/title_adapter/guest_memory.h>

#include <cstdint>

namespace sb::title_adapter {

// Reads the texture bindings out of a material's baked GX display list.
//
// This exists because the texture table a material packet carries is not, in general, the table the
// material was loaded with. `J3DModel` fills every packet's table pointer from the model data at
// construction (`J3DModel.cpp`, `mMatPackets[i].setTexture(pModelData->getTexture())`); a title may
// then hand the model an external material table read from a `.bmt`, and
// `J3DModelData::setMaterialTable` re-points *the model data* -- not the packets already built from
// it. The title then calls `initDL`, whose `J3DModel::makeDL` publishes the model data's current
// table and bakes each material's GX commands against it. From that moment the packet's own table
// pointer is a pre-swap snapshot that nothing reads, and the display list is the only record of
// what the material actually binds. GMSE01 does exactly this for the sky, for eight `MoveBG`
// objects and for the Mare townsfolk, so following the packet pointer resolves their textures
// against the placeholder images the model file shipped with.
//
// Reading the list also removes a second guess. A `ResTIMG` header states a sampler and a mip count
// that the material is free to override when it bakes; the list carries the values that reach the
// hardware, so nothing here has to reason about which of the two the title meant.
//
// The list is walked as GX itself walks it, and it refuses rather than resynchronises: a material
// display list contains register loads only, so a primitive opcode or an unknown one means the
// address or size was wrong and every binding recovered so far is unsound.

inline constexpr std::size_t GUEST_MAX_TEXMAPS = 8;

// `J3DDrawPacket::mpDisplayListObj`, and the two fields of the object it points at that `callDL`
// uses: `GXCallDisplayList(mpData[0], mSize)`.
inline constexpr GuestAddress GUEST_DRAW_PACKET_DISPLAY_LIST = 0x30;
inline constexpr GuestAddress GUEST_DISPLAY_LIST_DATA = 0x00;
inline constexpr GuestAddress GUEST_DISPLAY_LIST_SIZE = 0x08;

// A display list longer than this is taken as a bad address rather than a large material: the
// largest material list GMSE01 bakes is a few hundred bytes, and the refusal names the size it saw.
inline constexpr std::uint32_t GUEST_DISPLAY_LIST_MAX_BYTES = 64U * 1024U;

enum class GuestDisplayListError : std::uint8_t {
    None,
    NoReader,
    NullObject,
    UnreadableObject,
    NullList,
    EmptyList,
    ListTooLarge,
    Unreadable,
    Truncated,
    PrimitiveInMaterialList,
    UnknownOpcode,
};

[[nodiscard]] const char* name(GuestDisplayListError error) noexcept;

// One texmap as the list leaves it. `bound` is true only when both halves arrived: an image
// address and an image size/format. A texmap that a list mentions once and never completes is not a
// binding, and saying so is the difference between "this draw sampled the wrong image" and "this
// draw sampled no image".
struct GuestTexmapBinding {
    bool hasImage = false;
    bool hasFormat = false;
    std::uint32_t imageAddress = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t format = 0;
    std::uint8_t wrapS = 0;
    std::uint8_t wrapT = 0;
    // Both filters are stated as GX numbers them, which is what a resource header carries too: the
    // hardware encodes minification differently, and a consumer that had to know which of the two
    // it was holding would be holding two things.
    std::uint8_t magFilter = 0;
    std::uint8_t minFilter = 0;
    std::uint8_t mipCount = 1;
    // Where the palette for a colour-indexed format came from, matched from the texmap's TLUT name
    // to the TMEM offset a `TX_LOADTLUT` wrote. Zero when the format needs none or the list loaded
    // none.
    std::uint32_t paletteAddress = 0;
    std::uint16_t paletteEntries = 0;
    std::uint8_t paletteFormat = 0;

    [[nodiscard]] bool bound() const noexcept { return hasImage && hasFormat; }
};

// Denominators for every binding above: a list that bound nothing and a list that was never walked
// print the same eight empty texmaps otherwise.
struct GuestDisplayListTextures {
    GuestAddress address = 0;
    std::uint32_t bytes = 0;
    std::uint32_t commands = 0;
    std::uint32_t registerWrites = 0;
    std::uint32_t textureRegisterWrites = 0;
    GuestTexmapBinding texmap[GUEST_MAX_TEXMAPS]{};

    [[nodiscard]] std::uint32_t boundCount() const noexcept;
};

// Walks the list at `list` for `bytes` bytes.
[[nodiscard]] GuestDisplayListError
read_guest_display_list_textures(const GuestMemory& memory, GuestAddress list, std::uint32_t bytes,
                                 GuestDisplayListTextures& out) noexcept;

// Resolves `J3DDrawPacket::mpDisplayListObj` for a material packet and walks what it points at.
[[nodiscard]] GuestDisplayListError
read_guest_material_packet_textures(const GuestMemory& memory, GuestAddress materialPacket,
                                    GuestDisplayListTextures& out) noexcept;

} // namespace sb::title_adapter
