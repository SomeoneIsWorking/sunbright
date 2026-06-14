// =============================================================================
// Native heap bring-up — the runtime memory foundation for the PC port.
//
// WHY: the real decomp JKRExpHeap can't back native memory — JKRExpHeap::createRoot
// calls initArena (derefs GC low memory) and its CMemBlock block-manager uses
// 32-bit pointer math that breaks on 64-bit host addresses (proven by
// port/tests/heap_run.cpp). So the native runtime heap is a minimal JKRHeap whose
// alloc/free use the HOST allocator, installed as JKRHeap::sCurrentHeap (and
// sRootHeap). The engine's global operator new (real, in core) routes
//   operator new -> JKRHeap::alloc(static) -> sCurrentHeap->alloc()
// so once this is installed, every engine allocation runs natively.
//
// Call sb_heap_bringup() once at startup (before the engine allocates). It is
// idempotent. This is step 1 of the runtime bring-up (heap -> OS -> boot -> ...).
// =============================================================================
#include <JSystem/JKernel/JKRHeap.hpp>
#include <cstdlib>
#include <cstdint>
#include <new>

namespace {

// Minimal host-malloc-backed heap. Overrides JKRHeap's pure virtuals; no arena,
// no block accounting (host allocator owns that), no disposer tracking.
struct HostHeap : JKRHeap {
	HostHeap() : JKRHeap(nullptr, 0, nullptr, false) {}
	~HostHeap() override {}

	void* alloc(u32 size, int alignment) override {
		std::size_t a = (alignment <= 0) ? 8u : (std::size_t)alignment;
		std::size_t p2 = 8;
		while (p2 < a) p2 <<= 1;                       // power-of-two alignment
		std::size_t n = (size + p2 - 1) & ~(p2 - 1);   // size multiple of alignment
		return std::aligned_alloc(p2, n ? n : p2);
	}
	void free(void* ptr) override { std::free(ptr); }
	void freeAll() override {}
	void freeTail() override {}
	s32  resize(void*, u32) override { return -1; }
	s32  getSize(void*) override { return -1; }
	s32  getFreeSize() override { return 0x7FFFFFFF; }
	s32  getTotalFreeSize() override { return 0x7FFFFFFF; }
	u32  getHeapType() override { return 'HOST'; }
	bool check() override { return true; }
	bool dump() override { return true; }
};

// The root heap has PROCESS lifetime and is intentionally never destroyed —
// exactly as on GameCube, where the console never tears the root heap down (it
// runs no static/global destructors at power-off). Built with PLACEMENT NEW into
// a static buffer, for two reasons:
//   1. No atexit destructor. A function-local `static HostHeap` would register
//      one; at process exit ~JKRHeap runs its tree-teardown —
//      mChildTree.getParent()->removeChild(...) — which assumes a NON-root heap
//      in a populated tree. For a root heap getParent() is null (it was never
//      appended as anyone's child), so it calls JSUPtrList::remove on a null
//      `this` and segfaults. A placement-new object has no auto destructor.
//   2. No global operator new. The engine's `operator new` (in core) routes
//      through JKRHeap::alloc -> sCurrentHeap, which is NOT installed yet at
//      bring-up — so `new HostHeap()` would null-deref. Placement new does not
//      call operator new; it constructs in place in already-reserved storage.
HostHeap* g_root() {
	alignas(HostHeap) static unsigned char storage[sizeof(HostHeap)];
	static HostHeap* h = new (storage) HostHeap();
	return h;
}

} // namespace

// Install the host-backed heap as the current/root/system heap. Idempotent.
// (Declared extern "C" so the eventual native boot path can call it without
//  C++ name-mangling concerns.)
extern "C" void sb_heap_bringup() {
	if (JKRHeap::sCurrentHeap)
		return;
	HostHeap* h = g_root();
	JKRHeap::sCurrentHeap = h;
	JKRHeap::sRootHeap    = h;
	JKRHeap::sSystemHeap  = h;
}
