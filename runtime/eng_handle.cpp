#include "eng_handle.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

// =============================================================================
// Engine-object handle table — see eng_handle.h.
//
// Design constraint: sb_eng_host() is on the per-field-access HOT PATH (the
// recompiler emits it at EVERY typed engine-object load/store), so it must be
// lock-free. Allocation/release happen only on engine-object create/destroy
// (rare), so they may take a mutex.
//
//   * Reads (sb_eng_host): a single relaxed atomic load from a slot in a flat,
//     never-reallocated array — no lock, no map lookup. On x86/arm64 this is a
//     plain aligned pointer load.
//   * Writes (sb_eng_handle / sb_eng_release): guarded by g_mu; they touch the
//     reverse map (host->index) and the free list.
//
// The slot array is fixed-capacity and never reallocates, so a concurrent reader
// can never observe a moved/freed buffer. The capacity is generous (1M live
// engine handles — far beyond what SMS holds at once); exhausting it is a hard
// error (loud abort), never silent reuse of a live slot.
// =============================================================================

namespace {

constexpr u32 kCapacity = 1u << 20;  // 1,048,575 usable handles (index 0 reserved == null)

std::atomic<void*>* g_slots = nullptr;     // [kCapacity] host pointers; index 0 stays nullptr
std::atomic<u32>    g_high{1};             // high-water index (1.. ; 0 reserved)
std::mutex          g_mu;                  // guards the reverse map + free list (write path)
std::unordered_map<void*, u32> g_index;    // host ptr -> index (reverse lookup, write path only)
std::vector<u32>    g_free;                // recycled indices

void ensure_init() {
    // First writer initializes the slot array. The read path never runs before a
    // handle is allocated (you can only sb_eng_host() a token that was handed out).
    if (!g_slots) {
        auto* s = new std::atomic<void*>[kCapacity];
        for (u32 i = 0; i < kCapacity; ++i) s[i].store(nullptr, std::memory_order_relaxed);
        g_slots = s;
    }
}

}  // namespace

u32 sb_eng_handle(void* host) {
    if (!host) return 0;
    std::lock_guard<std::mutex> lk(g_mu);
    ensure_init();
    if (auto it = g_index.find(host); it != g_index.end())
        return kEngHandleBase | it->second;     // idempotent: same object -> same handle

    u32 idx;
    if (!g_free.empty()) { idx = g_free.back(); g_free.pop_back(); }
    else {
        idx = g_high.fetch_add(1, std::memory_order_relaxed);
        if (idx >= kCapacity) {
            std::fprintf(stderr, "FATAL: sb_eng_handle: handle table exhausted (%u)\n", kCapacity);
            std::abort();
        }
    }
    g_slots[idx].store(host, std::memory_order_release);
    g_index[host] = idx;
    return kEngHandleBase | idx;
}

void* sb_eng_host(u32 handle) {
    if (handle == 0) return nullptr;
    // Reject anything that is not one of our tokens (e.g. a real guest pointer
    // reaching here through a bug) -> nullptr, so the caller's deref faults loudly.
    if ((handle & ~kEngHandleMask) != kEngHandleBase) return nullptr;
    const u32 idx = handle & kEngHandleMask;
    if (!g_slots || idx >= kCapacity) return nullptr;
    return g_slots[idx].load(std::memory_order_acquire);
}

void sb_eng_release(void* host) {
    if (!host) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_slots) return;
    auto it = g_index.find(host);
    if (it == g_index.end()) return;            // never handled — no-op
    const u32 idx = it->second;
    g_slots[idx].store(nullptr, std::memory_order_release);
    g_index.erase(it);
    g_free.push_back(idx);
}

// Loud trap for an inline engine-field STORE (see intrinsics.h). The function-call boundary
// holds engine objects as handles and the recompiler does not yet emit field SETTERS; a guest
// MEM_W against a handle token would silently corrupt host or guest state. The generated code
// routes a typed inline store here so the bug surfaces as a named abort, not silent corruption.
void sb_eng_field_write_trap(const char* what, u32 handle) {
    std::fprintf(stderr,
        "FATAL: inline engine-field WRITE has no setter: %s (handle 0x%08x)\n"
        "  The game wrote a host engine object's field inline. Add a setter for it\n"
        "  (tools/recompiler/c_emitter.cpp) or bridge the writing function.\n",
        what ? what : "<unknown>", handle);
    std::abort();
}
