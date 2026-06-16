// render_test — the renderer's unit-test suite (the ngx equivalent of
// sunbright-recomp-test). Bottom-up TDD: each pure renderer unit asserts against
// SPEC-COMPUTED ground truth (hand-derived expected values), NOT against another
// renderer's pixels. Dolphin-free and self-contained so it runs in ctest with no
// ROM, no GPU, no running game.
//
// Why this exists: the renderer was built straight to "draw the whole scene and
// eyeball it," so fidelity bugs (the projection wash, dropped ortho) could never
// be made to go red/green and theories about them couldn't be falsified. This
// suite decomposes the renderer into testable units so a fix MOVES A NUMBER.
//
// Add a unit: write a `static int test_<unit>(char* rep, int cap)` returning the
// failing-case count (0 = pass), register it in kUnits below.

#include <cstdio>
#include <cstring>

// ── Units under test (self-test entry points defined in their own .cpp) ──────
extern int sb_ngx_vertex_selftest(char* out, int cap);   // runtime/ngx/ngx_vertex.cpp

namespace {

struct Unit { const char* name; int (*run)(char* rep, int cap); };

const Unit kUnits[] = {
    {"vertex_decode", sb_ngx_vertex_selftest},
};

}  // namespace

int main() {
    int total_fail = 0;
    char rep[8192];
    for (const Unit& u : kUnits) {
        rep[0] = '\0';
        int fails = u.run(rep, (int)sizeof rep);
        printf("[%s] %s\n", fails == 0 ? "PASS" : "FAIL", u.name);
        if (fails != 0) {
            fputs(rep, stdout);
            total_fail += fails;
        }
    }
    if (total_fail == 0) printf("render_test: all %zu units PASS\n", sizeof(kUnits)/sizeof(kUnits[0]));
    else                 printf("render_test: %d failing case(s)\n", total_fail);
    return total_fail == 0 ? 0 : 1;
}
