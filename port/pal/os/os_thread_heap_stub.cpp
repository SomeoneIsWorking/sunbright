// ===========================================================================
// port/pal/os/os_thread_heap_stub.cpp — PAL implementations for the GameCube
// OSAlloc heaps, the arena model, and the OS font API.
//
// Companion to os_pal.cpp (OS time / interrupts / OSReport), os_globals.cpp (the
// AT_ADDRESS low-RAM globals), and os_thread.cpp (the REAL native cooperative
// thread/message/mutex scheduler — threads & message queues used to be stubbed
// here; they now live there and actually run). This file owns ONLY:
//
//   OSAlloc   : OSInitAlloc, OSCreateHeap, OSDestroyHeap, OSAllocFromHeap,
//               OSFreeToHeap, OSCheckHeap, OSDumpHeap, OSReferentSize
//   Arena     : OSGetArenaHi, OSGetArenaLo, OSSetArenaHi, OSSetArenaLo
//   Font      : OSInitFont, OSGetFontEncode, OSGetFontTexture, OSGetFontWidth
//
// SIGNATURES: from <dolphin/os.h> (the shadow at port/compat/include, which
// externs the AT_ADDRESS globals and wraps the API in `extern "C"`), so every
// symbol resolves verbatim. os_pal.cpp already includes the same header, so
// this introduces no new multiple-definition risk (the globals are extern'd
// there; their single definitions live in os_globals.cpp).
//
// BEHAVIOR (bring-up placeholders — FLAG FOR PM):
//   - OSAlloc heaps: a thin wrapper over the host allocator. OSCreateHeap
//     returns a heap handle; OSAllocFromHeap == malloc, OSFreeToHeap == free.
//     OSReferentSize returns 0 (size not tracked). OSCheckHeap returns 0 (OK),
//     OSDumpHeap is a no-op. This is enough for callers that just want memory.
//   - Arena: a fixed host-allocated block models the GC arena so OSGetArenaLo/Hi
//     return a sane, non-overlapping [lo,hi) range and OSInitAlloc/OSSetArena*
//     behave. The arena is not the real GC memory map. FLAG: placeholder bounds.
//   - OS font: OSInitFont returns FALSE (no ROM font); OSGetFont* return null /
//     pass through with zeroed metrics. The native font path (JUTFont/RARC
//     fonts) replaces this. FLAG: ROM font unavailable.
// ===========================================================================

#include <dolphin/os.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

// --- minimal arena model -------------------------------------------------
// A fixed host block so OSGetArenaLo/Hi return a coherent, non-overlapping
// range. Bring-up placeholder bounds (NOT the real GC memory map).
constexpr std::size_t kArenaSize = 8u * 1024u * 1024u;  // 8 MiB
unsigned char* g_arena_block = nullptr;
void* g_arena_lo = nullptr;
void* g_arena_hi = nullptr;

void ensure_arena() {
    if (!g_arena_block) {
        g_arena_block = static_cast<unsigned char*>(std::malloc(kArenaSize));
        g_arena_lo = g_arena_block;
        g_arena_hi = g_arena_block ? (g_arena_block + kArenaSize) : nullptr;
    }
}

}  // namespace

extern "C" {

// NOTE: Threads, message queues, sleep/wakeup, and mutexes are NO LONGER stubbed
// here — they moved to os_thread.cpp (the real native cooperative scheduler).
// This file now owns ONLY the OSAlloc heaps, the arena model, and the OS font API.

// --------------------------------------------------------------- OSAlloc heaps
// OSHeapHandle is `int`. We hand back a single fixed handle; alloc/free wrap the
// host allocator (heap identity is ignored — the host heap is global).
void* OSInitAlloc(void* arenaStart, void* /*arenaEnd*/, int /*maxHeaps*/) {
    // Returns the new arena lo (the SDK reserves heap-array space at the front);
    // we have no real arena array, so pass the start through unchanged.
    return arenaStart;
}
int OSCreateHeap(void* /*start*/, void* /*end*/) {
    return 0;  // heap handle 0 (the host heap is global; identity unused)
}
void OSDestroyHeap(int /*heap*/) {}
void* OSAllocFromHeap(int /*heap*/, unsigned long size) {
    return std::malloc(static_cast<std::size_t>(size));
}
void OSFreeToHeap(int /*heap*/, void* ptr) { std::free(ptr); }
long OSCheckHeap(int /*heap*/) { return 0; }  // 0 == consistent / OK
void OSDumpHeap(int /*heap*/) {}
unsigned long OSReferentSize(void* /*ptr*/) {
    return 0;  // host malloc does not portably expose the size; not tracked.
}

// --------------------------------------------------------------- Arena
void* OSGetArenaHi(void) { ensure_arena(); return g_arena_hi; }
void* OSGetArenaLo(void) { ensure_arena(); return g_arena_lo; }
void OSSetArenaHi(void* newHi) { ensure_arena(); g_arena_hi = newHi; }
void OSSetArenaLo(void* newLo) { ensure_arena(); g_arena_lo = newLo; }

// --------------------------------------------------------------- OS font
BOOL OSInitFont(OSFontHeader* /*fontData*/) {
    return 0;  // FALSE: no GC ROM font on the PC port (native font path replaces).
}
u16 OSGetFontEncode(void) { return 0; }  // 0 == OS_FONT_ENCODE_ANSI
char* OSGetFontTexture(char* string, void** image, s32* x, s32* y, s32* width) {
    // No ROM font: report no glyph image, zero metrics, advance past one char.
    if (image) *image = nullptr;
    if (x) *x = 0;
    if (y) *y = 0;
    if (width) *width = 0;
    return string ? (*string ? string + 1 : string) : nullptr;
}
char* OSGetFontWidth(char* string, s32* width) {
    if (width) *width = 0;
    return string ? (*string ? string + 1 : string) : nullptr;
}

}  // extern "C"
