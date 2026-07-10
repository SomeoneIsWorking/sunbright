// stub_trace.h — shared FAIL-FAST instrumentation for SILENT LANDMINE stubs.
//
// Context (debug_journal/2026-07-10_stub_audit_fail_fast.md): sdk_stubs.cpp carried 20
// silent no-op bodies for JRenderer.cpp's exports because that file was wrongly excluded
// from the build; JRNISetTevOrder as a no-op left every TEV stage on GX_TEXMAP_NULL,
// rendering the whole 3D scene black for days before the excluded-file bug was found.
// A stub that produces valid-but-wrong state *silently* is the worst failure shape.
//
// SB_STUB_HIT(name) is the default conversion for a scaffold body that skips real game
// logic the caller assumes happened (state mutation, drawing, collision response, AI):
// it reports ONCE per call site (a static-local guard, not a per-frame spam) so boot
// keeps progressing but the unported surface is never silent. This is intentionally
// weaker than OSPanic — these bodies sit on gameplay classes (Enemy/MoveBG/etc.) not
// yet reached by the title/file-select boot path; panicking here would block boot
// fidelity work for behavior that doesn't run yet. Where a wrong return value would
// corrupt caller-visible STATE rather than just "this actor doesn't do anything" (see
// TMapObjTree::initMapObj in ring3_stubs.cpp), OSPanic is used directly instead of this
// macro — judged per function, not by blanket rule.
#pragma once
#include <dolphin/os.h>

#define SB_STUB_HIT(name) \
    do { \
        static bool sb_stub_hit_reported_ = false; \
        if (!sb_stub_hit_reported_) { \
            sb_stub_hit_reported_ = true; \
            OSReport("[STUB-CALLED] " name " -- unported, output will be wrong\n"); \
        } \
    } while (0)
