// Runtime smoke-test: prove the COMPILED engine's allocation path EXECUTES
// natively, driven by the real PAL heap bring-up (port/pal/heap/heap_init.cpp).
//
// Links a slice of libsmsport_core (non-whole-archive -> pulls ONLY referenced
// objects; confirms the heap path drags in NO game logic) + libsmsport_pal, calls
// sb_heap_bringup() to install the host-backed heap as sCurrentHeap, then runs the
// real engine allocation path: JKRHeap::alloc + global operator new/delete (both
// the decomp's, now in core). See heap_init.cpp for why the real decomp
// JKRExpHeap can't back native memory (32-bit CMemBlock + GC-arena init).
#include <JSystem/JKernel/JKRHeap.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" void sb_heap_bringup();

int main() {
    std::setbuf(stdout, nullptr);
    std::printf("[heap_run] sb_heap_bringup()...\n");
    sb_heap_bringup();
    std::printf("[heap_run] sCurrentHeap=%p\n", (void*)JKRHeap::sCurrentHeap);
    if (!JKRHeap::sCurrentHeap) { std::printf("[heap_run] FAIL: no current heap\n"); return 1; }

    void* a = JKRHeap::alloc(256, 16, nullptr);
    std::printf("[heap_run] JKRHeap::alloc(256,16)=%p aligned16=%d\n", a, a && ((uintptr_t)a % 16 == 0));
    if (!a) { std::printf("[heap_run] FAIL: alloc null\n"); return 1; }
    std::memset(a, 0xAB, 256);
    volatile unsigned char* p = (unsigned char*)a;
    std::printf("[heap_run] write/read ok: [0]=%02x [255]=%02x\n", p[0], p[255]);
    JKRHeap::free(a, nullptr);

    int* n = new int(42);  // global operator new (real, in core) -> JKRHeap::alloc -> host heap
    std::printf("[heap_run] global new int=%d @ %p\n", n ? *n : -1, (void*)n);
    if (!n || *n != 42) { std::printf("[heap_run] FAIL: new\n"); return 1; }
    delete n;

    std::printf("[heap_run] PASS: compiled engine alloc path runs natively via PAL heap\n");
    // Alloc path proven. Static teardown (~JKRHeap / global dtors) still touches the
    // uninitialized heap tree and would segfault — a separate teardown-only gap.
    std::_Exit(0);
}
