// lazy_singleton_test — spec-derived unit test for the TLightCommon "gp cache"
// lazy-populate shim (`sb_light_ary_or_search` / `sb_amb_ary_or_search` in
// reference/sms/src/MarioUtil/LightUtil.cpp). Locks in three properties the
// shim must satisfy for the setLight crash fix to hold:
//
//   1. Cached non-null pointer is returned WITHOUT invoking the searcher —
//      this is the RE-faithful hot path (no extra allocation on every
//      GX light frame).
//   2. Null cache invokes searcher once, writes result back into the cache,
//      and returns it. Subsequent calls use the cache (idempotence).
//   3. Searcher returning null keeps the cache null and the shim returns
//      null — the failure is not swallowed; upstream must still guard.
//
// Regressions this catches:
//   * Shim searching on EVERY call (would cost a NameRefGen hash walk per
//     GX_LIGHT0/1/2 setup — measurable and wrong).
//   * Shim caching the ALIAS not the value (`cache = search()` vs a copy) —
//     Would fail idempotence if search returns a temporary.
//   * Shim converting null result to a sentinel — would silently mask the
//     "no Light Group registered" case that a proper scene must report.
//   * Shim polarity flipped (search-when-non-null, return-when-null) — would
//     never cache and always crash.

#include "sms_boot_lazy_singleton.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

struct Dummy { int value; };

int main() {
    using sb::lazy_singleton::get_or_search;

    // 1. Cache HIT: search is NEVER called; the cached pointer is returned.
    Dummy hit_target{42};
    Dummy* cache = &hit_target;
    int search_calls = 0;
    Dummy* got = get_or_search(cache, [&]{ ++search_calls; return (Dummy*)nullptr; });
    CHECK(got == &hit_target,   "cache HIT returns the cached pointer");
    CHECK(cache == &hit_target, "cache HIT does NOT mutate cache");
    CHECK(search_calls == 0,    "cache HIT does NOT invoke searcher (hot path is free)");

    // 2. Cache MISS: search is called once, result is stored, second call
    //    returns without another search — idempotence.
    Dummy miss_target{99};
    cache = nullptr;
    search_calls = 0;
    got = get_or_search(cache, [&]{ ++search_calls; return &miss_target; });
    CHECK(got == &miss_target,   "cache MISS returns searcher result");
    CHECK(cache == &miss_target, "cache MISS writes searcher result BACK into cache");
    CHECK(search_calls == 1,     "cache MISS invokes searcher exactly ONCE");

    // Second call: should NOT re-search (would be broken if we cached the alias
    // instead of the value, or if we mis-ordered the write-back).
    got = get_or_search(cache, [&]{ ++search_calls; return &miss_target; });
    CHECK(got == &miss_target,   "second call after cache-fill returns cache");
    CHECK(search_calls == 1,     "second call does NOT re-invoke searcher");

    // 3. Failing search: null propagates, cache stays null, upstream MUST
    //    still guard against null (this is the "no Light Group in scene"
    //    case — the shim does NOT invent a sentinel).
    cache = nullptr;
    search_calls = 0;
    got = get_or_search(cache, [&]{ ++search_calls; return (Dummy*)nullptr; });
    CHECK(got == nullptr,      "failing search returns null");
    CHECK(cache == nullptr,    "failing search does NOT poison cache with sentinel");
    CHECK(search_calls == 1,   "failing search invoked exactly once");

    // Failing search called AGAIN — must re-attempt because the previous null
    // was a transient state (e.g. loadAfter hadn't run yet). Do NOT memoize
    // the failure.
    got = get_or_search(cache, [&]{ ++search_calls; return (Dummy*)nullptr; });
    CHECK(search_calls == 2,   "failing search RE-attempts on next call (null not memoized)");

    // 4. Polarity smoke-test: the shim MUST return the CACHE not the search
    //    result when cache is non-null (else it'd invoke search every time and
    //    the "hot path is free" property collapses). Set cache to a distinct
    //    pointer from search's return; assert cache wins.
    Dummy A{1}, B{2};
    cache = &A;
    search_calls = 0;
    got = get_or_search(cache, [&]{ ++search_calls; return &B; });
    CHECK(got == &A,          "polarity: non-null cache overrides search");
    CHECK(search_calls == 0,  "polarity: non-null cache short-circuits search");

    if (g_fail) { std::fprintf(stderr, "lazy_singleton_test: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("lazy_singleton_test: all passed\n");
    return 0;
}
