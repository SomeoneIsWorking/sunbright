// =============================================================================
// Super Mario Sunshine — native PC port: HEAP Platform Abstraction Layer.
//
// HISTORY: this file once provided minimal host-malloc-backed STUBS for the whole
// JKRHeap family + the global operator new/delete, "because we were NOT compiling
// the decomp's JKRHeap.cpp". As of the engine pointer-truncation wave, the REAL
// reference/sms TUs now compile into the core lib —
//   JKRHeap.cpp        -> JKRHeap statics, ctor/dtor, alloc/free, the non-pure
//                         virtuals, JKRDefaultMemoryErrorRoutine, AND the global
//                         operator new/new[]/delete/delete[] (the decomp's own).
//   JKRExpHeap.cpp     -> JKRExpHeap::create
//   JKRSolidHeap.cpp   -> JKRSolidHeap::create
// so all of those stubs were RETIRED here to avoid multiple-definition at link.
//
// What remains the PAL's job:
//   * JKRDisposer ctor/dtor — JKRDisposer.cpp is NOT yet in the core source set,
//     so provide a minimal version (no disposer-list registration). When
//     JKRDisposer.cpp is added to core_sources, retire this too.
//   * host_aligned_alloc/free — a host-backed aligned allocator kept available
//     for the runtime heap bring-up (the real JKRExpHeap/JKRSolidHeap need a
//     backing buffer + arena/OS init to function at RUNTIME; wiring that — vs.
//     pointing their alloc at the host allocator — is a runtime-bring-up task,
//     separate from this link milestone).
// =============================================================================

#include <JSystem/JKernel/JKRDisposer.hpp>
#include <cstdlib>
#include <cstdint>
#include <new>

// ---------------------------------------------------------------------------
// Host-backed aligned alloc/free (kept for runtime heap bring-up; see header).
// Over-allocates a small header so plain free() recovers the malloc base.
// ---------------------------------------------------------------------------
namespace {

[[maybe_unused]] void* host_aligned_alloc(std::size_t size, int alignment)
{
	std::size_t align = (alignment <= 0) ? 4u : (std::size_t)alignment;
	std::size_t a = sizeof(void*);
	while (a < align)
		a <<= 1;
	if (a < alignof(max_align_t))
		a = alignof(max_align_t);
	std::size_t total = size + a + sizeof(void*);
	void* base        = std::malloc(total);
	if (!base)
		return nullptr;
	std::uintptr_t raw     = reinterpret_cast<std::uintptr_t>(base) + sizeof(void*);
	std::uintptr_t aligned = (raw + (a - 1)) & ~(std::uintptr_t)(a - 1);
	reinterpret_cast<void**>(aligned)[-1] = base;
	return reinterpret_cast<void*>(aligned);
}

[[maybe_unused]] void host_aligned_free(void* ptr)
{
	if (!ptr)
		return;
	void* base = reinterpret_cast<void**>(ptr)[-1];
	std::free(base);
}

} // namespace

// ---------------------------------------------------------------------------
// JKRDisposer — minimal (JKRDisposer.cpp not yet in core). The real ctor links
// the object onto its root heap's disposer list (so freeAll can destruct it); we
// drop that tracking for bring-up. The typeinfo (_ZTI11JKRDisposer) the engine
// references is emitted because the out-of-line ~JKRDisposer is the key function.
// ---------------------------------------------------------------------------
JKRDisposer::JKRDisposer() : mRootHeap(nullptr), mPointerLinks(this) {}

JKRDisposer::~JKRDisposer() {}
