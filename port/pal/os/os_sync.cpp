// ===========================================================================
// port/pal/os/os_sync.cpp — portable PAL implementations of the GameCube OS
// mutex primitives the SMS engine references but that have no native hardware.
//
// WHY THIS EXISTS
// ---------------
// A link-surface scan of libsmsport_core.a leaves OSInitMutex / OSLockMutex /
// OSUnlockMutex UNDEFINED. They are the GameCube OS-kernel mutex wrappers, used
// by the engine to bracket critical sections (e.g. JKR heap allocation, the
// archive/aram managers). They are not engine code we recompile — they are the
// SDK's thin wrappers over the GC cooperative scheduler, which has no
// counterpart on the (currently single-threaded) PC port.
//
// FAITHFUL-FOR-BRING-UP BEHAVIOUR
// -------------------------------
// The native port is single-threaded during bring-up: there is exactly one
// thread of execution, so a mutex can never actually be contended. We therefore
// implement the SDK contract WITHOUT blocking:
//   OSInitMutex   — zero the struct to the SDK's initial state (count 0, no
//                   owner, empty wait queue), exactly as the real OSInitMutex
//                   does before any thread can touch it.
//   OSLockMutex   — recursive acquire: bump count (the real mutex is recursive;
//                   re-locking by the owner just increments count). Records the
//                   logical owner via a sentinel so OSTryLockMutex can answer.
//   OSUnlockMutex — release: decrement count, clear owner at 0. Never wakes a
//                   waiter because, single-threaded, there are none.
//   OSTryLockMutex— always succeeds (TRUE): single-threaded => never contended.
//                   (Declared in the header even though it is not yet on the
//                   undefined list — defining it here is harmless and keeps the
//                   primitive set complete for the next caller.)
//
// These are declared `extern "C"` by <dolphin/os/OSMutex.h>, so including that
// real header and defining against it gives them C linkage and verifies the
// signatures verbatim — a mismatch would fail the final link.
//
// WHEN REAL THREADING LANDS: replace the no-op body with a std::recursive_mutex
// (or the native scheduler's mutex) keyed off the OSMutex struct, and have
// OSTryLockMutex actually attempt the acquire. The OSMutex layout (queue /
// thread / count / link) is preserved here so a future real implementation can
// migrate without an ABI change.
// ===========================================================================

#include <dolphin/os/OSMutex.h>

#include <cstring>

extern "C" {

// A non-null sentinel "current thread" so the owner field is a stable,
// recognizable value during single-threaded bring-up (the real OSMutex stores
// the owning OSThread*; we have no live OSThread here, so we use a sentinel
// that is never dereferenced — only compared/cleared).
static OSThread* const kPalSentinelOwner =
    reinterpret_cast<OSThread*>(static_cast<uintptr_t>(0x1));

void OSInitMutex(OSMutex* mutex) {
    if (!mutex) {
        return;
    }
    // Faithful initial state: empty wait queue, no owner, count 0, no mutex-list
    // link. memset matches the SDK's "fresh mutex" all-zero starting point.
    std::memset(mutex, 0, sizeof(*mutex));
}

void OSLockMutex(OSMutex* mutex) {
    if (!mutex) {
        return;
    }
    // Recursive acquire. Single-threaded: never blocks. count tracks the
    // recursion depth exactly as the real OSLockMutex does for the owner.
    if (mutex->count == 0) {
        mutex->thread = kPalSentinelOwner;
    }
    ++mutex->count;
}

void OSUnlockMutex(OSMutex* mutex) {
    if (!mutex) {
        return;
    }
    // Release one level of recursion. Clear the owner when fully unlocked.
    // Guard against an unmatched unlock (count already 0) rather than going
    // negative — faithful to the SDK, which leaves an unlocked mutex unlocked.
    if (mutex->count > 0) {
        --mutex->count;
    }
    if (mutex->count == 0) {
        mutex->thread = nullptr;
    }
}

BOOL OSTryLockMutex(OSMutex* mutex) {
    if (!mutex) {
        return FALSE;
    }
    // Single-threaded: the lock can always be taken (either it is free, or this
    // same logical thread already owns it -> recursive re-lock). Acquire and
    // report success, matching the real OSTryLockMutex success path.
    if (mutex->count == 0) {
        mutex->thread = kPalSentinelOwner;
    }
    ++mutex->count;
    return TRUE;
}

}  // extern "C"
