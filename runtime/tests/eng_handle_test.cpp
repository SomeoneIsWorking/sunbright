// Unit test for the engine-object handle table (runtime/eng_handle.cpp) — the
// data half of the tailored-recomp boundary (docs/ARCHITECTURE_TARGET.md).
// Verifies the contract the recompiler's emitted code depends on: stable
// idempotent handles, correct round-trip, null/stale -> nullptr, defensive
// rejection of non-handle tokens, release + slot recycling, and concurrent
// lock-free reads alongside allocation.
#include "../eng_handle.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, msg) do { ++g_checks; if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s\n", (msg)); ++g_fail; } } while (0)

int main() {
    int a, b, c;
    void* pa = &a; void* pb = &b; void* pc = &c;

    // null maps both ways.
    CHECK(sb_eng_handle(nullptr) == 0, "null host -> handle 0");
    CHECK(sb_eng_host(0) == nullptr, "handle 0 -> nullptr");

    // round-trip + idempotent.
    u32 ha = sb_eng_handle(pa);
    u32 hb = sb_eng_handle(pb);
    CHECK(ha != 0 && hb != 0 && ha != hb, "distinct objects get distinct nonzero handles");
    CHECK(sb_eng_handle(pa) == ha, "idempotent: same object -> same handle");
    CHECK(sb_eng_host(ha) == pa, "round-trip pa");
    CHECK(sb_eng_host(hb) == pb, "round-trip pb");

    // tokens live in the reserved unmapped range (so a stray guest-MEM deref faults loudly).
    CHECK((ha & ~kEngHandleMask) == kEngHandleBase, "handle carries the reserved base tag");

    // a non-handle value (e.g. a real guest pointer 0x80123456) is rejected -> nullptr.
    CHECK(sb_eng_host(0x80123456u) == nullptr, "non-handle token -> nullptr (loud-deref defense)");
    CHECK(sb_eng_host(kEngHandleBase | 0x0FFFFFu) == nullptr, "never-allocated index -> nullptr");

    // release invalidates and recycles the slot; a new object reuses the freed index.
    sb_eng_release(pa);
    CHECK(sb_eng_host(ha) == nullptr, "released handle -> nullptr");
    u32 hc = sb_eng_handle(pc);
    CHECK((hc & kEngHandleMask) == (ha & kEngHandleMask), "freed slot index is recycled");
    CHECK(sb_eng_host(hc) == pc, "recycled handle maps to the new object");
    sb_eng_release(pa);  // double release (pa already freed) must be a safe no-op
    CHECK(sb_eng_host(hc) == pc, "double release of an old object does not disturb live handles");

    // Concurrent lock-free reads must stay correct while another thread allocates.
    // Pre-handle a fixed set, then read them on N threads while a writer churns.
    constexpr int kN = 256;
    static int objs[kN];
    std::vector<u32> hs(kN);
    for (int i = 0; i < kN; ++i) hs[i] = sb_eng_handle(&objs[i]);
    std::atomic<bool> stop{false};
    std::atomic<int> mismatches{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t)
        readers.emplace_back([&] {
            while (!stop.load()) for (int i = 0; i < kN; ++i)
                if (sb_eng_host(hs[i]) != &objs[i]) mismatches.fetch_add(1);
        });
    static int churn[4096];
    for (int i = 0; i < 4096; ++i) { u32 h = sb_eng_handle(&churn[i]); sb_eng_release(&churn[i]); (void)h; }
    stop.store(true);
    for (auto& th : readers) th.join();
    CHECK(mismatches.load() == 0, "concurrent reads of stable handles never tear");

    std::printf("eng_handle_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
