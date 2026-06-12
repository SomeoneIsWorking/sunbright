// 60 fps interpolation — object-level render decoupling (SUNBRIGHT_INTERP60=1).
//
// PC-port architecture (docs/model_interpolation.md, user-ruled 2026-06-12):
// the game simulates at 30 Hz; the RENDER pass runs every 60 Hz field. On the
// in-between field we re-issue the engine's own scene-graph draw pass through
// TMarDirector's perform lists and present it with a second display copy.
// No FIFO capture, no stream patching — the engine draws when we say.
//
// Stage 1 (this file, no blend yet): re-render the SAME frame on the
// in-between field. Visual no-op by construction; proves the draw pass
// re-issues cleanly (no double-tick of game state — anim/movement live in
// separate perform lists that we do not run) and that the second copy+present
// paces at one field each. Stage 2 redirects each J3DModel to draw matrices
// blended between tick N-1 and N (J3D double-buffers mDrawMtxBuf; see the
// design doc) — that turns this into real 60 fps motion.
//
// Frame anatomy (RE'd from reference/sms decomp + USA addresses verified in
// docs/decomp/mar_director_application.md):
//   TApplication::gameLoop: display->startRendering(); director->direct();
//                           display->endRendering();
//   direct() draw branch:   perform(0xffffffff, &TGraphics) over TMarDirector
//                           +0x40, +0x38, +0x3C, +0x1C GX, [+0x20 silhouette],
//                           +0x24 GXPost; then GXInvalidateTexAll.
//   endRendering (0x802f80d0): video->waitForRetrace(unk4C); IssueGXCopyDisp
//                           (EFB → XFB unk4[unkC], copy-clears EFB); unkC ^= 1.
// At 30 fps gameplay unk4C==2 (two fields per frame). We split it: super runs
// with unk4C=1 (one field + copy of the real frame), then the redraw pass,
// video->setNextXFB(new current buffer), and super again with unk4C=1 (second
// field + copy of the redraw) — same 2-field game cadence, two presents.
#include "../overrides.h"
#include "../intrinsics.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

float sunbright_cp_fill();                       // dolphin_hook.cpp — CP ring occupancy [0,1]
bool  sunbright_cp_drain_wait(float, int);       // bounded drain-wait for optional work

namespace {

constexpr u32 PERFORM_LIST_PERFORM = 0x802a4e28u;  // TPerformList::perform(u32, TGraphics*)
constexpr u32 GX_INVALIDATE_TEXALL = 0x80360400u;  // GXInvalidateTexAll
constexpr u32 VIDEO_SET_NEXT_XFB   = 0x802fc99cu;  // JDrama::TVideo::setNextXFB(const void*)
constexpr u32 MARDIR_DIRECT        = 0x80299838u;  // TMarDirector::direct (virtual)
constexpr u32 DISPLAY_END_RENDER   = 0x802f80d0u;  // JDrama::TDisplay::endRendering

// TMarDirector perform-list members (reference/sms include/System/MarDirector.hpp)
constexpr u32 kDrawLists[] = { 0x40, 0x38, 0x3C, 0x1C, 0x24 };
// +0x20 silhouette list is conditional in direct() (gpSilhouetteManager state);
// skipped in stage 1 — silhouettes absent on in-between fields only.

bool on() {
    static int v = -1;
    if (v < 0) v = getenv("SUNBRIGHT_INTERP60") ? 1 : 0;
    return v == 1;
}
bool dbg() {
    static int v = -1;
    if (v < 0) v = getenv("SUNBRIGHT_DBG_INTERP60") ? 1 : 0;
    return v == 1;
}

}  // namespace (reopened below)

// Redraw window state, consumed by the J3DModel::viewCalc override in
// interp_capture.cpp: while true, viewCalc substitutes blended draw matrices
// instead of running the guest body. Touched models are recorded for the
// pointer swap-back after the redraw (see the design doc: blend is written
// into buf0 in place, buf0/buf1 pointers swapped for the redraw only).
bool g_interp60_in_redraw = false;
std::vector<u32> g_interp60_touched;

namespace {

u32 g_mardir = 0;                 // TMarDirector* seen by the last direct()
unsigned long g_direct_stamp = 0, g_consumed_stamp = 0;
unsigned long g_redraws = 0, g_skip_rate = 0, g_skip_nodir = 0, g_skip_full = 0;

extern "C" void func_80299838(CPUState&);   // TMarDirector::direct
extern "C" void func_802f80d0(CPUState&);   // TDisplay::endRendering
extern "C" void func_802a4e28(CPUState&);   // TPerformList::perform

// The game's draw pass runs with the TGraphics that direct()'s CALC passes
// already populated (cameras/viewports fill viewport/projection/view there).
// A fabricated zeroed TGraphics feeds the draw pass degenerate state and
// corrupts the GX stream (verified live: FIFO Unknown Opcode right after the
// first redraw). So we snapshot the real one: when the game performs the GX
// list (mardir+0x1C), r5 is the live TGraphics — copy its 0x100 bytes.
u8  g_gfx_snap[0x100];
bool g_gfx_valid = false;

SUNBRIGHT_OVERRIDE(ov_interp_perform_snap, 0x802a4e28u) {
    if (on() && g_mardir && cpu.gpr[3] == MEM_R32(g_mardir + 0x1C) && cpu.gpr[5] >= 0x80000000u) {
        for (u32 i = 0; i < 0x100; i++) g_gfx_snap[i] = MEM_R8(cpu.gpr[5] + i);
        g_gfx_valid = true;
    }
    func_802a4e28(cpu);
}

// Mark "a gameplay frame was just drawn" — endRendering only inserts the
// in-between render for frames produced by TMarDirector (menus, logo and THP
// run other directors and never stamp).
SUNBRIGHT_OVERRIDE(ov_interp_mardir_direct, MARDIR_DIRECT) {
    g_mardir = cpu.gpr[3];
    func_80299838(cpu);
    g_direct_stamp++;
}

SUNBRIGHT_OVERRIDE(ov_interp_endRendering, DISPLAY_END_RENDER) {
    const u32 display = cpu.gpr[3];
    const bool fresh = g_direct_stamp != g_consumed_stamp;
    g_consumed_stamp = g_direct_stamp;

    const u16 wait = (u16)MEM_R16(display + 0x4C);
    if (on() && dbg()) {
        static int shown = 0;
        if (shown < 8) {
            fprintf(stderr, "[interp60] endRendering display=%08x wait=%u fresh=%d mardir=%08x\n",
                    display, wait, (int)fresh, g_mardir);
            shown++;
        }
    }
    // Admission control: the redraw doubles command volume; the CP ring can be
    // lapped by the CPU (GatherPipe overflow → the GPU reads half-overwritten
    // bytes = the Unknown-Opcode corruption pinned 2026-06-13). Insert the
    // OPTIONAL in-between frame only when the ring is under half full — else
    // present 30 fps for this frame, never corrupt.
    // A redraw bursts ~300 KB into the 512 KB ring; a point-sample of the fill
    // misses bursts (1 skip vs 70 overflows, 2026-06-13). Bounded drain-wait:
    // give the GPU up to 3 ms to get the ring under 25% (token delivery is
    // serviced inside, so a drawsync-parked GPU can retire and advance); on
    // timeout skip the redraw — this frame presents at 30 fps, never corrupts.
    const bool room = on() && fresh && g_mardir && g_gfx_valid && wait == 2
                      && sunbright_cp_drain_wait(0.25f, 3000);
    if (on() && fresh && !room) g_skip_full++;

    // Only a 2-field frame has an in-between field (gameplay). 1-field scenes
    // (60 fps menus) present every field already.
    if (!room) {
        if (on() && fresh && wait != 2) g_skip_rate++;
        if (on() && fresh && !g_mardir) g_skip_nodir++;
        func_802f80d0(cpu);
        return;
    }

    // First half: one field + the real frame's copy (EFB → XFB, copy-clear).
    MEM_W16(display + 0x4C, 1);
    cpu.gpr[3] = display;
    func_802f80d0(cpu);

    // The flip (unkC ^= 1) just ran — tell the VI the redraw will land in the
    // new current buffer, so the second present shows it.
    const u32 video = MEM_R32(display + 0x60);
    const u32 fb    = MEM_R32(display + 4 + 4u * (MEM_R16(display + 0xC) & 1));
    cpu.gpr[3] = video;
    cpu.gpr[4] = fb;
    call_ppc(cpu, VIDEO_SET_NEXT_XFB);

    // Stack forensics: the redraw runs the full scene draw pass DEEPER than the
    // game ever does (inside endRendering). If the calling thread's stack ends
    // adjacent to the CP FIFO ring (guest 0x80448d60..0x804c8d40), an overflow
    // stomps the ring = the observed Unknown-Opcode garbage. Measure headroom.
    if (dbg()) {
        const u32 cur = MEM_R32(0x800000E4u);
        const u32 se  = cur ? MEM_R32(cur + 0x308u) : 0;
        static u32 min_headroom = 0xFFFFFFFFu;
        const u32 headroom = cpu.gpr[1] - se;
        if (headroom < min_headroom) {
            min_headroom = headroom;
            fprintf(stderr, "[interp60] stack: sp=%08x stackEnd=%08x headroom=%u thread=%08x\n",
                    cpu.gpr[1], se, headroom, cur);
        }
    }

    // Re-issue the scene draw pass with the TGraphics snapshot taken while the
    // game performed this frame's GX list (see ov_interp_perform_snap). Stage 2
    // hooks J3DModel::viewCalc in this window to substitute blended matrices.
    const u32 saved_r1 = cpu.gpr[1];
    const u32 gfx = (saved_r1 - 0x110u) & ~0xFu;
    for (u32 i = 0; i < 0x100; i++) MEM_W8(gfx + i, g_gfx_snap[i]);
    cpu.gpr[1] = (gfx - 0x20u) & ~0xFu;

    // Optional bisect mask for the redraw lists (SUNBRIGHT_INTERP60_LISTS=hex
    // bitmask over kDrawLists indices; default all).
    static const u32 list_mask = [] {
        const char* e = getenv("SUNBRIGHT_INTERP60_LISTS");
        return e ? (u32)strtoul(e, nullptr, 16) : 0xFFu;
    }();
    // Blend window: viewCalc calls inside the pass substitute interpolated
    // matrices (interp_capture.cpp). SUNBRIGHT_INTERP60_NOBLEND=1 = A/B off.
    static const bool blend = !getenv("SUNBRIGHT_INTERP60_NOBLEND");
    g_interp60_touched.clear();
    g_interp60_in_redraw = blend;

    for (u32 li = 0; li < sizeof(kDrawLists) / sizeof(kDrawLists[0]); li++) {
        if (!(list_mask & (1u << li))) continue;
        const u32 list = MEM_R32(g_mardir + kDrawLists[li]);
        if (!list) continue;
        if (dbg() && g_redraws < 4)
            fprintf(stderr, "[interp60] redraw list %u (+0x%02x)\n", li, kDrawLists[li]);
        cpu.gpr[3] = list;
        cpu.gpr[4] = 0xFFFFFFFFu;
        cpu.gpr[5] = gfx;
        call_ppc(cpu, PERFORM_LIST_PERFORM);
    }
    call_ppc(cpu, GX_INVALIDATE_TEXALL);
    g_interp60_in_redraw = false;
    cpu.gpr[1] = saved_r1;

    // Swap the touched models' draw/nrm buffer pointers back so the next real
    // viewCalc's swap sees the game's own buffer parity (buf1 = frame N stays
    // the next blend's source; the blend memory becomes N+1's write target —
    // the same double-buffer lifetime the game relies on).
    for (u32 model : g_interp60_touched) {
        const u32 view = MEM_R32(model + 0x7C);
        for (u32 base : { 0x60u, 0x68u }) {           // mDrawMtxBuf / mNrmMtxBuf
            const u32 a0 = MEM_R32(model + base), a1 = MEM_R32(model + base + 4);
            if (!a0 || !a1) continue;
            const u32 p0 = MEM_R32(a0 + 4 * view), p1 = MEM_R32(a1 + 4 * view);
            MEM_W32(a0 + 4 * view, p1);
            MEM_W32(a1 + 4 * view, p0);
        }
    }

    const float fill_after_lists = sunbright_cp_fill();

    // Second half: one more field + the redraw's copy — the in-between present.
    MEM_W16(display + 0x4C, 1);
    cpu.gpr[3] = display;
    func_802f80d0(cpu);

    if (dbg()) {
        const float fill_end = sunbright_cp_fill();
        static int logged = 0;
        if (logged < 6 || fill_after_lists > 0.75f || fill_end > 0.75f) {
            fprintf(stderr, "[interp60] fill: after_lists=%.2f after_present=%.2f\n",
                    fill_after_lists, fill_end);
            if (logged >= 6 && (fill_after_lists > 0.95f || fill_end > 0.95f))
                fprintf(stderr, "[interp60] RING NEAR FULL during redraw #%lu\n", g_redraws);
            logged++;
        }
    }

    MEM_W16(display + 0x4C, wait);   // restore the game's retrace count
    g_redraws++;
    if (dbg() && (g_redraws <= 4 || (g_redraws % 256) == 0))
        fprintf(stderr, "[interp60] redraws=%lu skip_rate=%lu skip_nodir=%lu skip_full=%lu\n",
                g_redraws, g_skip_rate, g_skip_nodir, g_skip_full);
}

}  // namespace
