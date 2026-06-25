// boot_heap_bringup.cpp — early heap for static-initializer allocations in sms-boot.
//
// The decomp's global operator new (JKRHeap::alloc) replaces the standard one for the
// WHOLE executable — including glslang's STATIC-INITIALIZER allocations (the KeywordMap
// unordered_map in Scan.cpp), which run BEFORE main() (so before PlatformInit brings up
// the game's real heap). Without a current heap those allocs deref a null sCurrentHeap
// and SEGV. Bring a heap up in a high-priority static initializer (runs before glslang's
// unprioritized ctors); the game's TApplication::initialize() later createRoot()s its own
// root heap and becomeCurrentHeap()s, so this only services the pre-main static allocs.
// (Same pattern as native/render/tests/j3dmesh_test.cpp.)
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <cstdlib>
#include <cstring>
#include <new>

// Mark the calling host thread (the main fiber) as a game thread so its plain `new`
// allocations route onto the JKR heap — the default is FALSE so foreign GL-driver
// threads fall back to host malloc (JKRHeap.cpp). This MUST run before any game
// allocation on the main thread; the init_priority below guarantees that (it runs
// before glslang's unprioritized static-init allocations).
extern "C" void sb_mark_game_thread(void);

namespace {
void bringup_heap(size_t bytes) {
    void* block = nullptr;
    if (posix_memalign(&block, 32, bytes) != 0 || !block) std::abort();
    std::memset(block, 0, bytes);
    u32 ehSize = (u32)((sizeof(JKRExpHeap) + 0xF) & ~0xFu);
    new (block) JKRExpHeap((u8*)block + ehSize, (u32)bytes - ehSize, nullptr, false);
}
struct HeapBringup { HeapBringup() { sb_mark_game_thread(); bringup_heap(64u * 1024 * 1024); } };
HeapBringup g_heap_bringup __attribute__((init_priority(101)));
} // namespace
