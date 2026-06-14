// ===========================================================================
// port/pal/os/os_thread_heap_stub.cpp — PAL stubs for the GameCube OS threads,
// message queues, OSAlloc heaps, arena, and OS font API.
//
// Companion to os_pal.cpp (OS time / interrupts / mutexes / OSReport) and
// os_globals.cpp (the AT_ADDRESS low-RAM globals). This file intentionally does
// NOT touch either of those (per the PAL task constraint) — it adds ONLY the
// remaining OS* symbols left undefined at the smsport_main link:
//
//   Threads   : OSCreateThread, OSExitThread, OSCancelThread, OSDetachThread,
//               OSResumeThread, OSSleepThread, OSWakeupThread, OSInitThreadQueue,
//               OSIsThreadTerminated, OSGetCurrentThread, OSGetThreadPriority
//   Messages  : OSInitMessageQueue, OSSendMessage, OSReceiveMessage
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
// BEHAVIOR (bring-up placeholders — FLAG FOR PM): the PC port does NOT yet run
// the GameCube cooperative scheduler. Real threading for the engine's worker
// threads is provided elsewhere (the recompiler track's native scheduler; the
// port track's port/src/JKRThread.cpp wrapper). These are LINK-time stubs:
//
//   - OSCreateThread returns 1 (success) but spawns NOTHING; the thread body
//     never runs. OSResumeThread/OSExitThread/OSCancel/Detach are no-ops.
//     OSGetCurrentThread returns nullptr (no current OS thread). OSGetThread
//     Priority returns 0. OSIsThreadTerminated returns TRUE (treat as already
//     finished, so a join/wait loop terminates rather than spinning).
//     FLAG: any subsystem that depends on a spawned OS thread actually running
//     will not function until native threading is wired in.
//   - Message queues: a minimal, correct ring-buffer FIFO (no blocking — the
//     NOBLOCK semantics). Send returns FALSE when full, Receive FALSE when
//     empty; both return TRUE and move the message otherwise. This is real,
//     portable behavior (not a no-op) so single-threaded producer/consumer
//     handoff works; with no scheduler, BLOCK-mode senders/receivers degrade to
//     non-blocking (they get FALSE rather than sleeping). FLAG: cross-thread
//     blocking handoff is not honored without native threading.
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

// --------------------------------------------------------------- Threads
int OSCreateThread(OSThread* /*thread*/, void* (* /*func*/)(void*),
                   void* /*param*/, void* /*stack*/, u32 /*stackSize*/,
                   s32 /*priority*/, u16 /*attr*/) {
    return 1;  // "created" — but no real thread is spawned (see header).
}
void OSExitThread(OSThread* /*thread*/) {}
void OSCancelThread(OSThread* /*thread*/) {}
void OSDetachThread(OSThread* /*thread*/) {}
long OSResumeThread(OSThread* /*thread*/) { return 0; }
void OSSleepThread(OSThreadQueue* /*queue*/) {}
void OSWakeupThread(OSThreadQueue* /*queue*/) {}
void OSInitThreadQueue(OSThreadQueue* queue) {
    // Zero the queue head/tail so it is a defined empty queue.
    if (queue) std::memset(queue, 0, sizeof(*queue));
}
BOOL OSIsThreadTerminated(OSThread* /*thread*/) {
    return 1;  // TRUE: treat as finished so join/wait loops terminate.
}
OSThread* OSGetCurrentThread(void) { return nullptr; }
long OSGetThreadPriority(OSThread* /*thread*/) { return 0; }

// --------------------------------------------------------------- Messages
// A correct non-blocking ring FIFO over the SDK OSMessageQueue layout. The
// struct's msgArray/msgCount/firstIndex/usedCount fields are filled by
// OSInitMessageQueue; we operate on them directly so producer/consumer handoff
// works without faking hardware. (No scheduler -> BLOCK flag degrades to
// non-blocking; see header.)
void OSInitMessageQueue(struct OSMessageQueue* mq, void* msgArray,
                        long msgCount) {
    if (!mq) return;
    std::memset(mq, 0, sizeof(*mq));
    mq->msgArray = msgArray;        // struct field is void* (array of OSMessage)
    mq->msgCount = msgCount;
    mq->firstIndex = 0;
    mq->usedCount = 0;
}
int OSSendMessage(struct OSMessageQueue* mq, void* msg, long /*flags*/) {
    if (!mq || !mq->msgArray || mq->msgCount <= 0) return 0;
    if (mq->usedCount >= mq->msgCount) return 0;  // full (non-blocking)
    long idx = (mq->firstIndex + mq->usedCount) % mq->msgCount;
    static_cast<OSMessage*>(mq->msgArray)[idx] = msg;  // OSMessage == void*
    mq->usedCount++;
    return 1;
}
int OSReceiveMessage(struct OSMessageQueue* mq, void* msg, long /*flags*/) {
    if (!mq || !mq->msgArray || mq->usedCount <= 0) return 0;  // empty
    if (msg)
        *static_cast<OSMessage*>(msg) =
            static_cast<OSMessage*>(mq->msgArray)[mq->firstIndex];
    mq->firstIndex = (mq->firstIndex + 1) % mq->msgCount;
    mq->usedCount--;
    return 1;
}

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
