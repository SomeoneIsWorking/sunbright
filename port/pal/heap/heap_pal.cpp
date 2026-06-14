// =============================================================================
// Super Mario Sunshine — native PC port: HEAP Platform Abstraction Layer.
//
// MINIMAL bring-up heap. The decomp routes nearly every allocation through
// JKRHeap and the global operator new/delete (declared in JKRHeap.hpp). On the
// GameCube those are backed by JKRExpHeap / JKRSolidHeap carved out of the
// OS arena. For the native PC port we back the WHOLE thing with the host
// allocator (malloc / aligned_alloc / free): no arena, no block accounting, no
// disposer tracking, no group ids. Just enough that the engine can allocate,
// write, read and release memory natively.
//
// This file provides the out-of-line definitions for the symbols a link scan
// (`nm -u libsmsport_core.a`) reports as undefined in the heap area, with
// signatures EXACTLY matching the decomp declarations in
//   reference/sms/include/JSystem/JKernel/JKRHeap.hpp
//   reference/sms/include/JSystem/JKernel/JKRDisposer.hpp
//   reference/sms/include/JSystem/JKernel/JKRSolidHeap.hpp
//   reference/sms/include/JSystem/JKernel/JKRExpHeap.hpp
// A signature mismatch (mangled name) would simply fail to satisfy the
// undefined reference, so these MUST match byte-for-byte.
//
// === What maps to what ===
//   JKRHeap::alloc(size, align, heap)         -> heap (or sCurrentHeap)->alloc()
//   JKRHeap (concrete) ::alloc(size, align)   -> aligned host alloc
//   JKRHeap::free(ptr)                         -> host free
//   JKRHeap::free(ptr, heap) (static)          -> host free
//   global operator new / new[]                -> JKRHeap::alloc (align 4)
//   operator new(size, JKRHeap*, int)          -> JKRHeap::alloc from that heap
//   operator new(size, int)                    -> JKRHeap::alloc (that align)
//   global operator delete / delete[]          -> host free
//   JKRExpHeap::create / JKRSolidHeap::create  -> new PortHeap (host-backed)
//   JKRDisposer ctor/dtor                      -> minimal (NO disposer-list link)
//   sCurrentHeap                               -> a singleton host-backed heap
//
// === Limitations (intentional, minimal bring-up) ===
//   * No arena / fixed-size pool: allocations are unbounded host malloc.
//   * No per-heap accounting: getFreeSize/getTotalFreeSize report a large
//     sentinel; getSize(ptr) cannot recover the original request and returns -1.
//   * free() is a real host free, but resize()/freeTail()/freeAll() are no-ops
//     (we do not track live blocks). check()/dump() are trivial.
//   * No JKRDisposer list: ctor/dtor do NOT register/unregister on a root heap
//     (the disposer-tracking machinery is out of scope for the bring-up).
//   * createRoot/createSolidHeap on a fixed buffer ignore the buffer and use
//     the host allocator.
//
// === GLOBAL OPERATOR NEW/DELETE WARNING ===
//   The global operator new/new[]/delete/delete[] defined here REPLACE the
//   default ones for the ENTIRE port binary that links this object. That is
//   intentional — the decomp DECLARES these globals (JKRHeap.hpp) and expects
//   them to route through JKRHeap. But every C++ allocation in the final binary
//   (including any non-decomp host code linked alongside) will go through here.
//   Since we forward to malloc/free this is behaviourally compatible with the
//   default global new/delete (it just adds the JKRHeap bookkeeping hook), but
//   be aware it is process-global. The sized delete forms (operator delete(p, n)
//   from C++14) forward to the same free.
// =============================================================================

#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include <JSystem/JKernel/JKRDisposer.hpp>

// NOTE on JKRExpHeap: the vendored reference/sms JKRExpHeap.hpp contains an
// inline (CMemBlock::getBlock) with a GameCube-era `(u32)data` pointer cast that
// is a HARD -fpermissive error on a 64-bit host (a known, deferred 64-bit-port
// truncation bug in the decomp header — see port/README "Pointer->u32/s32
// truncation"). The core lib masks it with -w + that file isn't a core source.
// We keep THIS PAL strict (no -fpermissive, the README rule) by NOT including
// that header and instead declaring the minimal JKRExpHeap interface we need to
// define JKRExpHeap::create. The class is single-inheritance from JKRHeap, so a
// JKRExpHeap* is ABI-identical here and at the engine's call sites; the mangled
// symbol (_ZN12JKRExpHeap6createEjP7JKRHeapb) matches regardless of the omitted
// members. Do NOT add the real members here — callers only use the JKRHeap base
// through the returned pointer.
class JKRExpHeap : public JKRHeap {
public:
	static JKRExpHeap* create(u32 size, JKRHeap* parent, bool errorFlag);
};

#include <cstdlib>
#include <cstring>
#include <new>

// ---------------------------------------------------------------------------
// Host-backed aligned alloc/free. aligned_alloc requires the size be a multiple
// of the alignment and the alignment be a power of two >= sizeof(void*); we
// normalise both. We over-allocate a small header so plain `free` works on the
// returned pointer regardless of the alignment offset.
// ---------------------------------------------------------------------------
namespace {

void* host_aligned_alloc(std::size_t size, int alignment)
{
	std::size_t align = (alignment <= 0) ? 4u : (std::size_t)alignment;
	// Round alignment up to a power of two, at least alignof(max_align_t).
	std::size_t a = sizeof(void*);
	while (a < align)
		a <<= 1;
	if (a < alignof(max_align_t))
		a = alignof(max_align_t);

	// We store the malloc base pointer just before the aligned address so the
	// matching free can recover it. Allocate size + alignment + header.
	std::size_t total = size + a + sizeof(void*);
	void* base        = std::malloc(total);
	if (!base)
		return nullptr;
	std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(base) + sizeof(void*);
	std::uintptr_t aligned = (raw + (a - 1)) & ~(std::uintptr_t)(a - 1);
	// stash base immediately before the aligned pointer
	reinterpret_cast<void**>(aligned)[-1] = base;
	return reinterpret_cast<void*>(aligned);
}

void host_aligned_free(void* ptr)
{
	if (!ptr)
		return;
	void* base = reinterpret_cast<void**>(ptr)[-1];
	std::free(base);
}

} // namespace

// ---------------------------------------------------------------------------
// PortHeap: the one concrete JKRHeap the port uses. JKRHeap declares its alloc/
// free (and most of the heap interface) pure-virtual, so a concrete class is
// required for `sCurrentHeap->alloc(...)` to dispatch. Every virtual is given a
// minimal host-backed body.
//
// NOTE: we deliberately do NOT route construction through the real
// JKRHeap(void*,u32,JKRHeap*,bool) ctor (which would pull in OSInitMutex / the
// system-heap/current-heap registration / disposer machinery). We provide a
// minimal JKRHeap ctor below instead.
// ---------------------------------------------------------------------------
namespace {

class PortHeap : public JKRHeap {
public:
	PortHeap() : JKRHeap(nullptr, 0, nullptr, false) {}
	~PortHeap() override {}

	void* alloc(u32 size, int alignment) override
	{
		return host_aligned_alloc(size, alignment);
	}
	void free(void* ptr) override { host_aligned_free(ptr); }
	void freeAll() override {}
	void freeTail() override {}
	s32 resize(void* /*ptr*/, u32 /*size*/) override { return -1; }
	s32 getSize(void* /*ptr*/) override { return -1; }
	s32 getFreeSize() override { return 0x7FFFFFFF; }
	s32 getTotalFreeSize() override { return 0x7FFFFFFF; }
	u32 getHeapType() override { return 'PORT'; }
	bool check() override { return true; }
	bool dump() override { return true; }
};

// The singleton current/root heap. Constructed on first use (the very first
// allocation), so static-init order with the rest of the engine does not
// matter. Returned by value of its address into JKRHeap::sCurrentHeap.
PortHeap* g_port_heap()
{
	static PortHeap heap;
	return &heap;
}

} // namespace

// ---------------------------------------------------------------------------
// JKRHeap static data members (defined in the decomp's JKRHeap.cpp which we are
// NOT compiling — provide them here).
// ---------------------------------------------------------------------------
JKRHeap* JKRHeap::sSystemHeap;
JKRHeap* JKRHeap::sCurrentHeap;
JKRHeap* JKRHeap::sRootHeap;
JKRHeapErrorHandler* JKRHeap::mErrorHandler;
void* JKRHeap::mCodeStart;
void* JKRHeap::mCodeEnd;
void* JKRHeap::mUserRamStart;
void* JKRHeap::mUserRamEnd;
u32 JKRHeap::mMemorySize;

// ---------------------------------------------------------------------------
// Minimal JKRHeap base ctor/dtor. Unlike the decomp, this does NOT init a mutex,
// register a system/current heap, or link into a parent tree — just stores the
// span. The member subobjects (mChildTree/mDisposerList) construct via their own
// (header/JSUList.cpp) ctors. The JKRDisposer base ctor below is also minimal.
// ---------------------------------------------------------------------------
JKRHeap::JKRHeap(void* data, u32 size, JKRHeap* /*parent*/, bool errorFlag)
    : JKRDisposer()
    , mChildTree(this)
    , mDisposerList()
{
	mSize      = size;
	mStart     = (u8*)data;
	mEnd       = (u8*)data + size;
	mErrorFlag = errorFlag;
	mInitFlag  = false;
}

JKRHeap::~JKRHeap() {}

// Non-pure JKRHeap virtuals declared out-of-line in the decomp's JKRHeap.cpp
// (which we do not compile). They populate the JKRHeap/PortHeap vtable, so the
// linker needs a body for each. Minimal: group ids and state-tracking are part
// of the heap-accounting machinery we drop, so these are no-ops/identity.
s32 JKRHeap::changeGroupID(u8 /*newGroupId*/) { return 0; }
u8 JKRHeap::getCurrentGroupId() { return 0; }
void JKRHeap::state_register(JKRHeap::TState* /*state*/, u32 /*id*/) const {}
bool JKRHeap::state_compare(const JKRHeap::TState& /*a*/,
                            const JKRHeap::TState& /*b*/) const
{
	return true;
}
void JKRHeap::state_dump(const JKRHeap::TState& /*state*/) const {}

// freeAll has a non-pure (overridable) default in the header but no out-of-line
// body in what we compile; the decomp's iterates the disposer list. Minimal: no
// disposers tracked, so nothing to free.
void JKRHeap::freeAll() {}

// ---------------------------------------------------------------------------
// JKRHeap::alloc / free (static) — the funnel the global new/delete use. Matches
// the decomp semantics: dispatch to the explicit heap, else sCurrentHeap.
// We lazily install the port heap as the current heap on first use.
// ---------------------------------------------------------------------------
void* JKRHeap::alloc(u32 byteCount, int padding, JKRHeap* heap)
{
	if (!sCurrentHeap)
		sCurrentHeap = g_port_heap();
	if (heap)
		return heap->alloc(byteCount, padding);
	if (sCurrentHeap)
		return sCurrentHeap->alloc(byteCount, padding);
	return host_aligned_alloc(byteCount, padding);
}

void JKRHeap::free(void* memory, JKRHeap* heap)
{
	if (heap)
		heap->free(memory);
	else
		host_aligned_free(memory);
}

// ---------------------------------------------------------------------------
// JKRExpHeap::create / JKRSolidHeap::create — return a host-backed PortHeap. The
// fixed `size` and parent are ignored (no arena). createRoot also makes one.
// We allocate a PortHeap on the host (so it can be `delete`d) and cast: the
// engine only uses the JKRHeap interface on the result.
// ---------------------------------------------------------------------------
JKRExpHeap* JKRExpHeap::create(u32 /*size*/, JKRHeap* /*parent*/, bool /*errorFlag*/)
{
	PortHeap* h = new PortHeap();
	if (!JKRHeap::sCurrentHeap)
		JKRHeap::sCurrentHeap = h;
	if (!JKRHeap::sRootHeap)
		JKRHeap::sRootHeap = h;
	if (!JKRHeap::sSystemHeap)
		JKRHeap::sSystemHeap = h;
	return reinterpret_cast<JKRExpHeap*>(h);
}

JKRSolidHeap* JKRSolidHeap::create(u32 /*size*/, JKRHeap* /*parent*/, bool /*errorFlag*/)
{
	PortHeap* h = new PortHeap();
	if (!JKRHeap::sCurrentHeap)
		JKRHeap::sCurrentHeap = h;
	if (!JKRHeap::sRootHeap)
		JKRHeap::sRootHeap = h;
	if (!JKRHeap::sSystemHeap)
		JKRHeap::sSystemHeap = h;
	return reinterpret_cast<JKRSolidHeap*>(h);
}

// ---------------------------------------------------------------------------
// JKRDisposer — minimal. The real ctor links the object onto its root heap's
// disposer list (so freeAll can destruct it); we drop that tracking. The
// typeinfo (_ZTI11JKRDisposer) the engine references is emitted because this
// class has a key function (the out-of-line ~JKRDisposer) defined here.
// ---------------------------------------------------------------------------
JKRDisposer::JKRDisposer() : mRootHeap(nullptr), mPointerLinks(this) {}

JKRDisposer::~JKRDisposer() {}

// ---------------------------------------------------------------------------
// JKRDefaultMemoryErrorRoutine — declared in JKRHeap.hpp; referenced by the
// real JKRHeap ctor (which we replaced, so likely unused here, but provide it
// to be safe / match the declaration).
// ---------------------------------------------------------------------------
void JKRDefaultMemoryErrorRoutine(void* /*heap*/, u32 /*size*/, int /*alignment*/) {}

// ---------------------------------------------------------------------------
// Global operator new / delete (declared in JKRHeap.hpp). These REPLACE the
// process-global new/delete for the whole port binary (see warning at top).
// All route through JKRHeap::alloc / host free, matching the decomp bodies.
// ---------------------------------------------------------------------------
void* operator new(std::size_t byteCount)
{
	return JKRHeap::alloc((u32)byteCount, 4, nullptr);
}
void* operator new(std::size_t byteCount, int alignment)
{
	return JKRHeap::alloc((u32)byteCount, alignment, nullptr);
}
void* operator new(std::size_t byteCount, JKRHeap* heap, int alignment)
{
	return JKRHeap::alloc((u32)byteCount, alignment, heap);
}

void* operator new[](std::size_t byteCount)
{
	return JKRHeap::alloc((u32)byteCount, 4, nullptr);
}
void* operator new[](std::size_t byteCount, int alignment)
{
	return JKRHeap::alloc((u32)byteCount, alignment, nullptr);
}
void* operator new[](std::size_t byteCount, JKRHeap* heap, int alignment)
{
	return JKRHeap::alloc((u32)byteCount, alignment, heap);
}

void operator delete(void* memory) noexcept { host_aligned_free(memory); }
void operator delete[](void* memory) noexcept { host_aligned_free(memory); }
// C++14 sized-deallocation forms (the link scan reports _ZdlPvm / _ZdaPvm):
void operator delete(void* memory, std::size_t) noexcept { host_aligned_free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { host_aligned_free(memory); }
