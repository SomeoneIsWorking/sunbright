// amiking_test — spec-constant regression test for TAmiKing::loadAfter's particle
// registration. Path + ID pair — a typo in either would silently miss the .jpa load and
// the Pinna Park boss would spawn without its effect. Guards the pair.

#include "sms_boot_amiking.h"
#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

int main() {
    // Path must match the DOL string at 0x803920b4 exactly.
    CHECK(std::strcmp(sb::kAmiKingParticlePath, "/scene/mapObj/amiking.jpa") == 0,
          "path must match DOL string exactly");
    // Common typos to guard: leading "/scene/mapObj" (not "/scene/mapObjs"), lowercase
    // filename (not "AmiKing.jpa"), trailing .jpa (not .jpa1 / .jpc).
    CHECK(sb::kAmiKingParticlePath[0] == '/', "path is absolute (leading /)");

    // ID must be exactly 0x184 — mismatch would break the runtime resource lookup.
    CHECK(sb::kAmiKingParticleId == 0x184, "particle id must be 0x184");
    // High halfword must be 0 (Ami King is a per-scene ID in the low band; if a future
    // edit pushes to 0x1840 or similar, the runtime table would OOB).
    CHECK((sb::kAmiKingParticleId >> 8) == 0x01, "id high byte is 0x01");
    CHECK((sb::kAmiKingParticleId & 0xFF) == 0x84, "id low byte is 0x84");

    if (g_fail) { std::fprintf(stderr, "amiking_test: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("amiking_test: all passed\n");
    return 0;
}
