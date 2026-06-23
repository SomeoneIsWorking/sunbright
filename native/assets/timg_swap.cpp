// timg_swap.cpp — see timg_swap.h.
#include "timg_swap.h"
#include <cstdint>
#include <unordered_set>

namespace smsport::assets {
namespace {

// ResTIMG layout (ResTIMG.hpp, 0x20 bytes): u8 format@0x00, u8 alphaEnabled@0x01,
// u16 width@0x02, u16 height@0x04, u8 wrapS@0x06, u8 wrapT@0x07, u8 isIndexTexture@0x08,
// u8 colorFormat@0x09, u16 numColors@0x0A, u32 paletteOffset@0x0C, u8 fields@0x10..0x19,
// s16 lodBias@0x1A, u32 imageDataOffset@0x1C. Only the multibyte header scalars swap.
inline void sw16(uint8_t* p) { uint8_t t = p[0]; p[0] = p[1]; p[1] = t; }
inline void sw32(uint8_t* p) {
    uint8_t t = p[0]; p[0] = p[3]; p[3] = t;
    t = p[1]; p[1] = p[2]; p[2] = t;
}

// Pointers already swapped (idempotency). Archive buffers stay mounted for the screen's
// lifetime, so a pointer uniquely identifies a TIMG; swapping twice would re-corrupt it.
std::unordered_set<const void*>& swapped_set() {
    static std::unordered_set<const void*> s;
    return s;
}

} // namespace

void restimg_swap_to_host(const void* timg) {
    if (!timg) return;
    if (!swapped_set().insert(timg).second) return;   // already swapped
    uint8_t* p = static_cast<uint8_t*>(const_cast<void*>(timg));
    sw16(p + 0x02);   // width
    sw16(p + 0x04);   // height
    sw16(p + 0x0A);   // numColors
    sw32(p + 0x0C);   // paletteOffset
    sw16(p + 0x1A);   // lodBias (s16)
    sw32(p + 0x1C);   // imageDataOffset
}

}  // namespace smsport::assets
