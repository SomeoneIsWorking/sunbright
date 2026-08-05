// diag_vptr.cpp — SBR_VPTR_DUMP=1: ground truth for "which vtable does a live TViewObj carry?"
//
// WHY. The 60fps interpolation override must decide, from a bare guest address, whether an object
// is a JDrama::TActor before it writes a transform to `guest + 0x10`. Getting that wrong on a
// non-actor corrupts whatever that object keeps there. tools/re/us_vtables.py recovers candidate
// TActor vtables statically, but TActor is multiply derived (TPlacement, JStage::TActor), so a class
// has SEVERAL vtables and only the PRIMARY — the one a `+0x00` vptr actually points at — is a valid
// membership key. Seeding an allowlist with secondary tables yields an all-false-negative test that
// looks fully populated.
//
// That question is not worth reasoning about from multiple-inheritance layout rules. The running
// game knows the answer: dump the vptr each object actually carries. This is the project's own rule
// that a discriminator must be RUN against both classes, not argued about.
//
// WHAT IT PRINTS. One line per DISTINCT vptr — the vptr, the NameRef name of the first object seen
// carrying it, and how many objects shared it. That gives both halves of the check:
//   * are the recovered addresses present at all (are they primary tables?), and
//   * do the vptrs that appear belong to objects whose NAMES are actors?
// A vptr that never appears here is not a TActor key, whatever the static scan believed.
//
// It observes only: the real body always runs, and nothing guest-side is written.

#include "overrides.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdlib>
#include <cstring>

extern "C" void func_802fcc94(CPUState&);      // JDrama::TViewObj::testPerform(u32, TGraphics*)

namespace {

bool enabled() {
    static const bool on = std::getenv("SBR_VPTR_DUMP") != nullptr;
    return on;
}

constexpr int MAX_VPTRS = 512;

struct Seen {
    u32  vptr = 0;
    long count = 0;
    char name[64] = {0};
};

Seen g_seen[MAX_VPTRS];
int  g_n = 0;
bool g_overflow = false;     // more distinct vptrs than slots — say so, never silently drop
long g_objects = 0;
long g_noName  = 0;          // objects whose name pointer was unreadable
bool g_reported = false;

// TNameRef: vptr @ 0x00, const char* mName @ 0x04 (decomp include/JSystem/JDrama/JDRNameRef.hpp).
void read_name(u32 obj, char* out, size_t cap) {
    out[0] = '\0';
    const u32 p = sb_r32(obj + 4);
    if (!sb_ram_fast(p)) { ++g_noName; std::strncpy(out, "<name unreadable>", cap - 1); return; }
    size_t i = 0;
    for (; i + 1 < cap; ++i) {
        const u8 c = sb_r8(p + (u32)i);
        if (c == 0) break;
        out[i] = (char)c;
    }
    out[i] = '\0';
}

void note(u32 obj) {
    const u32 vptr = sb_r32(obj);
    ++g_objects;
    for (int i = 0; i < g_n; ++i) {
        if (g_seen[i].vptr == vptr) { ++g_seen[i].count; return; }
    }
    if (g_n >= MAX_VPTRS) { g_overflow = true; return; }
    Seen& s = g_seen[g_n++];
    s.vptr = vptr;
    s.count = 1;
    read_name(obj, s.name, sizeof s.name);
}

void report() {
    lucent::info("vptr", "DISTINCT vptrs carried by live TViewObjs: {} over {} dispatches{}",
                 g_n, g_objects,
                 g_overflow ? "  [TABLE OVERFLOWED -- some vptrs dropped]" : "");
    lucent::info("vptr", "  names unreadable for {} objects (counted, not skipped silently)",
                 g_noName);
    for (int i = 0; i < g_n; ++i)
        lucent::info("vptr", "  0x{:08x}  n={:<6} {}", g_seen[i].vptr, g_seen[i].count,
                     g_seen[i].name);
}

void vptr_dump(CPUState& cpu) {
    if (!enabled()) { func_802fcc94(cpu); return; }

    const u32 obj = (u32)cpu.gpr[3];
    if (sb_ram_fast(obj)) note(obj);

    // One report, after enough dispatches that the scene is populated. Reporting every frame would
    // bury the answer; reporting once at exit would lose it if the run is killed by timeout.
    if (!g_reported && g_objects > 200000) {
        g_reported = true;
        report();
    }

    func_802fcc94(cpu);
}

} // namespace

SB_OVERRIDE(0x802fcc94, vptr_dump, "JDrama::TViewObj::testPerform",
            "diagnostic only (SBR_VPTR_DUMP): dump the vtable pointer each live TViewObj carries, "
            "to tell PRIMARY vtables from secondary ones; always runs the real body")
