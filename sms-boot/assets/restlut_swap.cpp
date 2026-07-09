// restlut_swap.cpp — see restlut_swap.h.
#include "restlut_swap.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <new>

namespace smsport::assets {
namespace {

// ResTLUT layout (JUTPalette.hpp): u8 format@0x00, u8 transparency@0x01,
// u16 numColors@0x02. Only the multibyte header scalar (numColors) swaps.
inline void sw16(uint8_t* p) { uint8_t t = p[0]; p[0] = p[1]; p[1] = t; }

// Node storage MUST come from std::malloc, NOT the global operator new — see
// timg_swap.cpp's MallocAlloc comment for why (this set is a process-global
// that must outlive JKR heap freeAll()s).
template <class T> struct MallocAlloc {
    using value_type = T;
    MallocAlloc() = default;
    template <class U> MallocAlloc(const MallocAlloc<U>&) noexcept {}
    T* allocate(std::size_t n) {
        void* p = std::malloc(n * sizeof(T));
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept { std::free(p); }
    template <class U> bool operator==(const MallocAlloc<U>&) const noexcept { return true; }
    template <class U> bool operator!=(const MallocAlloc<U>&) const noexcept { return false; }
};

// CONTENT-VERIFIED idempotency (same policy as restimg_swap_to_host): snapshot
// the swapped header bytes alongside the pointer so a recycled address (after
// a JKR heap freeAll()) that now holds a NEW big-endian ResTLUT is detected as
// "different occupant" and re-swapped, rather than skipped as "already done".
struct HeaderSnapshot {
    uint8_t bytes[4];
};
using SwapMap = std::unordered_map<const void*, HeaderSnapshot, std::hash<const void*>,
                                   std::equal_to<const void*>,
                                   MallocAlloc<std::pair<const void* const, HeaderSnapshot>>>;
SwapMap& swapped_map() {
    static SwapMap s;
    return s;
}

} // namespace

void restlut_swap_to_host(const void* tlut) {
    if (!tlut) return;
    uint8_t* p = static_cast<uint8_t*>(const_cast<void*>(tlut));
    auto [it, fresh] = swapped_map().try_emplace(tlut);
    if (!fresh && std::memcmp(it->second.bytes, p, 4) == 0)
        return; // same swapped occupant — already host-endian
    sw16(p + 0x02);   // numColors
    std::memcpy(it->second.bytes, p, 4);
}

}  // namespace smsport::assets
