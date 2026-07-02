// bathtub_test — spec-constant test for TBathtub::loadAfter's four particle
// registrations (@0x801fb894). Path + ID pairs are the only observable outputs of
// the port, so this pins them exactly against the DOL rodata / disasm.

#include "sms_boot_bathtub.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

int main() {
    using namespace sb::bathtub_load_after;

    // Path strings must match the DOL rodata exactly (0x803941b0 / 0x803941d0 /
    // 0x803941f0 / 0x80394214) — a typo in either the folder ("mapObj" vs "map")
    // or the filename (yuge1 vs yuge2) silently misses the .jpa file.
    CHECK(std::strcmp(kSteamPath,    "/scene/map/map/ms_lkp_yuge1.jpa") == 0, "steam path exact");
    CHECK(std::strcmp(kFountainPath, "/scene/map/map/ms_kp_funsui.jpa") == 0, "fountain path exact");
    CHECK(std::strcmp(kBreakAPath,   "/scene/map/map/ms_kp_break_a.jpa") == 0, "break_a path exact");
    CHECK(std::strcmp(kBreakBPath,   "/scene/map/map/ms_kp_break_b.jpa") == 0, "break_b path exact");

    // All paths are absolute, hit the /scene/map/map/ tree, and use the ms_ prefix.
    for (const char* p : {kSteamPath, kFountainPath, kBreakAPath, kBreakBPath}) {
        CHECK(p[0] == '/',                                              "absolute path (leading /)");
        CHECK(std::strncmp(p, "/scene/map/map/ms_", 18) == 0,           "belongs to /scene/map/map/ms_ tree");
    }

    // The steam path is the ONLY one with the "lkp_" (large-koopa?) prefix —
    // guards a copy-paste typo where the port loads "ms_kp_yuge1.jpa" instead.
    CHECK(std::strstr(kSteamPath,    "lkp_") != nullptr, "steam is lkp_ (long prefix)");
    CHECK(std::strstr(kFountainPath, "lkp_") == nullptr, "fountain is kp_ (not lkp_)");
    CHECK(std::strstr(kBreakAPath,   "lkp_") == nullptr, "break_a is kp_ (not lkp_)");

    // IDs — the guard bytes at 0x8040d068 + offset = id (each id doubles as its
    // own guard's low byte). Reversing any pair would silently mis-key the runtime
    // resource table.
    CHECK(kSteamId    == 0x1be, "steam id 0x1be");
    CHECK(kFountainId == 0x1bf, "fountain id 0x1bf");
    CHECK(kBreakAId   == 0xf6,  "break_a id 0xf6");
    CHECK(kBreakBId   == 0xf7,  "break_b id 0xf7");

    // Group invariants — the two pairs live in different id ranges.
    CHECK(kSteamId    >= 0x100 && kFountainId >= 0x100, "yuge/funsui in high band");
    CHECK(kBreakAId   <  0x100 && kBreakBId   <  0x100, "break_{a,b} in low band");

    // No duplicates — a copy-paste that leaves two IDs identical would silently
    // overwrite one .jpa slot at runtime.
    const std::uint16_t all[4] = { kSteamId, kFountainId, kBreakAId, kBreakBId };
    for (int i = 0; i < 4; ++i) for (int j = i + 1; j < 4; ++j) {
        CHECK(all[i] != all[j], "no duplicate particle IDs");
    }

    if (g_fail) { std::fprintf(stderr, "bathtub_test: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("bathtub_test: all passed\n");
    return 0;
}
