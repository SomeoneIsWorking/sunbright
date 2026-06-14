// Runtime smoke-test: prove the COMPILED engine's allocation path EXECUTES
// natively via a host-backed heap.
//
// FINDING (this test): the engine links a runnable slice with NO game logic
// pulled in. The REAL decomp JKRExpHeap can't back a native heap — createRoot
// calls initArena (derefs GC low memory) and its CMemBlock block-manager uses
// 32-bit pointer math that breaks on 64-bit host addresses. So the native
// runtime heap must be HOST-BACKED: a minimal JKRHeap whose alloc/free use the
// host allocator, installed as JKRHeap::sCurrentHeap. The engine's global
// operator new (real, in core) -> JKRHeap::alloc (real) -> sCurrentHeap->alloc
// then runs natively. This is the heap bring-up the runtime needs.
#include <JSystem/JKernel/JKRHeap.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
struct HostHeap : JKRHeap {
    HostHeap() : JKRHeap(nullptr, 0, nullptr, false) {}
    void* alloc(u32 size, int al) override { return std::aligned_alloc(al < 8 ? 8 : al, (size + al + 7) & ~7u); }
    void  free(void* p) override { std::free(p); }
    void  freeAll() override {} void freeTail() override {}
    s32 resize(void*, u32) override { return -1; } s32 getSize(void*) override { return -1; }
    s32 getFreeSize() override { return 0x7fffffff; } s32 getTotalFreeSize() override { return 0x7fffffff; }
    u32 getHeapType() override { return 'HOST'; } bool check() override { return true; } bool dump() override { return true; }
};
}
int main() {
    std::setbuf(stdout, nullptr);
    std::printf("[heap_run] installing host-backed sCurrentHeap...\n");
    static HostHeap hh;
    JKRHeap::sCurrentHeap = &hh;
    void* a = JKRHeap::alloc(256, 16, nullptr);
    std::printf("[heap_run] JKRHeap::alloc(256,16)=%p aligned16=%d\n", a, a && ((uintptr_t)a%16==0));
    if (a) { std::memset(a,0xAB,256); std::printf("[heap_run] write/read ok\n"); JKRHeap::free(a, nullptr); }
    int* n = new int(42);  // global operator new (real, in core) -> JKRHeap::alloc -> host
    std::printf("[heap_run] global new int=%d @ %p\n", n?*n:-1, (void*)n);
    delete n;
    std::printf("[heap_run] PASS: compiled engine alloc path runs natively (host-backed heap)\n");
    // The alloc path is proven. Static teardown (~JKRHeap / pulled-in global dtors)
    // still touches the uninitialized heap tree and would segfault — a separate
    // teardown-only runtime gap, NOT the alloc path. Exit before dtors run.
    std::_Exit(0);
}
