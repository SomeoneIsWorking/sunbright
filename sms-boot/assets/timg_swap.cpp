// timg_swap.cpp — see timg_swap.h.
#include "timg_swap.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include <new>
#include <unistd.h>
#include <sys/syscall.h>
static inline int sb_gettid() { return (int)::syscall(SYS_gettid); }

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

// Node storage MUST come from std::malloc, NOT the global operator new. On the game thread
// the global `new` routes plain allocations into the game's JKR arena (JKRHeap::alloc); the
// game then wipes that arena wholesale on every scene transition (JKRHeap::freeAll/destroy).
// This set is a PROCESS-GLOBAL that outlives any single scene, so arena-backed nodes would be
// freed under its feet — leaving dangling buckets that crash the NEXT insert (observed:
// title→file-select transition freed the title's nodes → SIGSEGV in _M_emplace during the
// option-screen J2DPicture loads). A malloc-backed allocator keeps the set's storage on the
// host heap, isolated from the game's heap lifecycle — the same policy JKRHeap.cpp applies to
// all host-runtime bookkeeping.
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

// Pointers already swapped (idempotency). Archive buffers stay mounted for the screen's
// lifetime, so a pointer uniquely identifies a TIMG; swapping twice would re-corrupt it.
using SwapSet = std::unordered_set<const void*, std::hash<const void*>,
                                   std::equal_to<const void*>, MallocAlloc<const void*>>;
SwapSet& swapped_set() {
    static SwapSet s;
    return s;
}

} // namespace

void restimg_swap_to_host(const void* timg) {
    if (!timg) return;
    if (std::getenv("SB_TIMG_DBG")) {
        static long n = 0;
        std::fprintf(stderr, "[timg] enter #%ld tid=%d ptr=%p\n",
                     n++, sb_gettid(), timg);
        std::fflush(stderr);
    }
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
