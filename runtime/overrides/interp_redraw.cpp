// 60 fps interpolation — object-level render decoupling (SUNBRIGHT_INTERP60=1).
//
// PC-port architecture (docs/model_interpolation.md, user-ruled 2026-06-12):
// the game simulates at 30 Hz; the RENDER pass runs every 60 Hz field. On the
// in-between field we re-issue the engine's own scene-graph draw pass through
// TMarDirector's perform lists and present it with a second display copy.
// No FIFO capture, no stream patching — the engine draws when we say.
//
// On the in-between field we re-issue the scene-graph draw pass with each
// J3DModel's draw matrices BLENDED between tick N-1 and N (J3D double-buffers
// mDrawMtxBuf; the blend is applied in the viewCalc override, interp_capture.cpp)
// — real 60 fps motion. No double-tick of game state: anim/movement live in
// separate perform lists we do not run; only the draw lists re-issue, and the
// second copy+present paces at one field.
//
// The whole path is instrumented and driveable LIVE over HTTP at /interp60
// (interp60_probe below) — counters, the does-the-blend-reach-the-GPU
// cross-check, and live alpha/blend/perturb A/B controls. No rebuild cycle.
#include "../overrides.h"
#include "../intrinsics.h"
#include "../interp60.h"
#include "../gx_stream.h"
#include "../gx_parse.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Live debug + control state (declared in interp60.h).
Interp60Dbg g_i60;

namespace {

constexpr u32 PERFORM_LIST_PERFORM = 0x802a4e28u;  // TPerformList::perform(u32, TGraphics*)
constexpr u32 GX_INVALIDATE_TEXALL = 0x80360400u;  // GXInvalidateTexAll
constexpr u32 VIDEO_SET_NEXT_XFB   = 0x802fc99cu;  // JDrama::TVideo::setNextXFB(const void*)
constexpr u32 MARDIR_DIRECT        = 0x80299838u;  // TMarDirector::direct (virtual)
constexpr u32 DISPLAY_END_RENDER   = 0x802f80d0u;  // JDrama::TDisplay::endRendering

// TMarDirector perform-list members (reference/sms include/System/MarDirector.hpp),
// in direct()'s draw order. +0x20 is the silhouette/shadow list — it MUST be
// re-issued on the in-between field too, or shadows render only on real fields
// and flicker at 30 Hz. A null list ptr (silhouette manager inactive) is skipped.
constexpr u32 kDrawLists[] = { 0x40, 0x38, 0x3C, 0x1C, 0x20, 0x24 };

}  // namespace (reopened below)

// SSOT: is 60 fps interpolation enabled? (declared in runtime/interp60.h)
bool sunbright_interp60() {
    static int v = -1;
    if (v < 0) v = getenv("SUNBRIGHT_INTERP60") ? 1 : 0;
    return v == 1;
}

// Redraw window state, consumed by the J3DModel::viewCalc override in
// interp_capture.cpp: while true, viewCalc substitutes blended draw matrices
// into mDrawMtxBuf[1][view] and skips the guest body.
bool g_interp60_in_redraw = false;
void interp60_restore_after_redraw();   // interp_capture.cpp
void interp60_take_motion();            // interp_capture.cpp
void interp60_registry_clear();         // interp_capture.cpp
void interp60_blend_registry();         // interp_capture.cpp

namespace {

u32 g_mardir = 0;                 // TMarDirector* seen by the last direct()
unsigned long g_direct_stamp = 0, g_consumed_stamp = 0;

extern "C" void func_80299838(CPUState&);   // TMarDirector::direct
extern "C" void func_802f80d0(CPUState&);   // TDisplay::endRendering
extern "C" void func_802a4e28(CPUState&);   // TPerformList::perform

// Snapshot of the live TGraphics the game's draw pass used (cameras/viewports
// fill viewport/projection/view there). A fabricated zeroed TGraphics corrupts
// the GX stream, so we copy the real one when the game performs the GX list.
u8  g_gfx_snap[0x100];
bool g_gfx_valid = false;

SUNBRIGHT_OVERRIDE(ov_interp_perform_snap, 0x802a4e28u) {
    if (sunbright_interp60() && g_mardir && cpu.gpr[3] == MEM_R32(g_mardir + 0x1C) && cpu.gpr[5] >= 0x80000000u) {
        for (u32 i = 0; i < 0x100; i++) g_gfx_snap[i] = MEM_R8(cpu.gpr[5] + i);
        g_gfx_valid = true;
    }
    func_802a4e28(cpu);
}

// TJ3DSysSetViewMtx::perform (0x80296a50) copies gfx->mViewMtx into j3dSys. Count
// how often it runs during the redraw — if 0, the view-blend never reaches j3dSys
// and camera interp can't work via this path (would need a direct j3dSys write).
extern "C" void func_80296a50(CPUState&);
SUNBRIGHT_OVERRIDE(ov_interp_setviewmtx, 0x80296a50u) {
    if (g_interp60_in_redraw) g_i60.setviewmtx_calls++;
    func_80296a50(cpu);
}

SUNBRIGHT_OVERRIDE(ov_interp_mardir_direct, MARDIR_DIRECT) {
    g_mardir = cpu.gpr[3];
    if (g_i60.mode == 3) interp60_registry_clear();   // start a fresh model registry
    func_80299838(cpu);                                // populates it via real-field viewCalc
    g_direct_stamp++;
}

SUNBRIGHT_OVERRIDE(ov_interp_endRendering, DISPLAY_END_RENDER) {
    const u32 display = cpu.gpr[3];
    const bool fresh = g_direct_stamp != g_consumed_stamp;
    g_consumed_stamp = g_direct_stamp;

    const u16 wait = (u16)MEM_R16(display + 0x4C);

    // Publish liveness for the probe.
    g_i60.direct_stamp = g_direct_stamp;
    g_i60.mardir = g_mardir;
    g_i60.gfx_valid = g_gfx_valid;

    // Insert the in-between field only for a fresh 2-field gameplay frame drawn by
    // TMarDirector with a valid TGraphics snapshot. 1-field scenes (60 fps menus)
    // present every field already.
    const bool room = sunbright_interp60() && fresh && g_mardir && g_gfx_valid && wait == 2;
    if (!room) {
        if (sunbright_interp60() && fresh && wait != 2) g_i60.skip_rate++;
        if (sunbright_interp60() && fresh && !g_mardir)  g_i60.skip_nodir++;
        if (sunbright_interp60() && fresh && !g_gfx_valid) g_i60.skip_full++;
        func_802f80d0(cpu);
        return;
    }

    // XFB buffer index BEFORE the first present's flip: the real frame copies to
    // buffer[old_idx], the redraw to buffer[old_idx^1].
    const u32 old_idx = MEM_R16(display + 0xC) & 1;

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

    // Re-issue the scene draw pass with the TGraphics snapshot taken while the
    // game performed this frame's GX list. viewCalc inside the pass substitutes
    // blended matrices (interp_capture.cpp).
    const u32 saved_r1 = cpu.gpr[1];
    const u32 gfx = (saved_r1 - 0x110u) & ~0xFu;
    for (u32 i = 0; i < 0x100; i++) MEM_W8(gfx + i, g_gfx_snap[i]);
    cpu.gpr[1] = (gfx - 0x20u) & ~0xFu;

    // ── mode 2: CAMERA interpolation (blend the j3dSys view matrix) ──
    // The live camera view matrix is the j3dSys global at 0x804045DC (RE'd from
    // TJ3DSysSetViewMtx::perform: it MTXCopy's gfx->mViewMtx into 0x80400000+17884).
    // The GX-list TGraphics we snapshot has a ZERO view there; the real view lives
    // in j3dSys, set by a view node in an earlier list. We blend N-1->N of j3dSys's
    // view, write it BOTH to j3dSys directly AND into our gfx copy at +0xB4 (so the
    // view node, which re-runs in our re-issued lists, propagates the blend rather
    // than resetting j3dSys to a stale/zero value). viewCalc (mode 2, not skipped)
    // then recomputes every model's draw matrix against the interpolated camera.
    constexpr u32 J3DSYS_VIEWMTX = 0x804045DCu;
    float saved_jview[12];
    {
        static float prev_view[12];
        float cur_view[12];
        for (u32 i = 0; i < 12; i++) cur_view[i] = mem_rf32(J3DSYS_VIEWMTX + i * 4);
        for (u32 i = 0; i < 12; i++) saved_jview[i] = cur_view[i];   // restore after redraw
        g_i60.view_x = cur_view[3]; g_i60.view_y = cur_view[7]; g_i60.view_z = cur_view[11];
        g_i60.view_snaps++;
        if (g_i60.mode == 2 && g_i60.have_prev_view) {
            const float a = g_i60.alpha;
            g_i60.view_dx = cur_view[3]  - prev_view[3];
            g_i60.view_dy = cur_view[7]  - prev_view[7];
            g_i60.view_dz = cur_view[11] - prev_view[11];
            for (u32 i = 0; i < 12; i++) {
                float v = (1.0f - a) * prev_view[i] + a * cur_view[i];
                if (g_i60.perturb && (i == 3)) v = cur_view[i] + 300.0f;  // unmissable A/B
                mem_wf32(J3DSYS_VIEWMTX + i * 4, v);   // j3dSys directly
                mem_wf32(gfx + 0xB4 + i * 4, v);       // and the gfx the view node reads
            }
        }
        for (u32 i = 0; i < 12; i++) prev_view[i] = cur_view[i];
        g_i60.have_prev_view = 1;
    }

    // ── instrument: measure the in-between field's actual GX render volume ──
    g_i60.setviewmtx_calls = 0;
    g_i60.blended_addrs.clear();
    g_i60.track_addr = 0;
    const unsigned long long bytes0 = gxs_decoded_bytes();
    const unsigned long      runs0  = gxs_decode_runs();

    g_interp60_in_redraw = true;
    // mode 3: blend EVERY registered model's draw-matrix double-buffer toward N-1
    // before the draw lists re-issue (the viewCalc override skips recompute in this
    // mode so the blend survives). This is what reaches the whole scene.
    if (g_i60.mode == 3) interp60_blend_registry();
    for (u32 li = 0; li < sizeof(kDrawLists) / sizeof(kDrawLists[0]); li++) {
        const u32 list = MEM_R32(g_mardir + kDrawLists[li]);
        if (!list) continue;
        g_i60.cur_list = (int)li;
        cpu.gpr[3] = list;
        cpu.gpr[4] = 0xFFFFFFFFu;
        cpu.gpr[5] = gfx;
        call_ppc(cpu, PERFORM_LIST_PERFORM);
    }
    g_i60.cur_list = -1;
    call_ppc(cpu, GX_INVALIDATE_TEXALL);
    g_interp60_in_redraw = false;
    cpu.gpr[1] = saved_r1;

    // Restore j3dSys's view matrix to tick N (we blended it for the redraw).
    if (g_i60.mode == 2 && g_i60.have_prev_view)
        for (u32 i = 0; i < 12; i++) mem_wf32(J3DSYS_VIEWMTX + i * 4, saved_jview[i]);

    // Clobber check: is the blend we wrote still in RAM now that all draw lists ran?
    if (g_i60.track_addr) g_i60.track_after = mem_rf32(g_i60.track_addr + 3 * 4);

    // Second half: one more field + the redraw's copy — the in-between present.
    MEM_W16(display + 0x4C, 1);
    cpu.gpr[3] = display;
    func_802f80d0(cpu);

    // GX volume + decode-run delta for the whole in-between field.
    g_i60.redraw_gx_bytes = gxs_decoded_bytes() - bytes0;
    g_i60.redraw_gx_runs  = gxs_decode_runs()  - runs0;

    (void)old_idx;

    // DECISIVE cross-check: the redraw frame's copy just rotated g_prev_info.
    // Count how many POS-matrix array bases (CP array 12) the GX stream set point
    // at a buffer we blended — i.e. how much of the GPU's matrix input is ours.
    {
        const GxFrameInfo& fi = gxs_prev_frame_info();
        unsigned long seen = 0, hit = 0;
        // GXSetArray stores a PHYSICAL base (virtual & 0x03FFFFFF); blended_addrs
        // are virtual — compare on the low 26 bits.
        g_i60.s_base = g_i60.s_addr = 0;
        for (const auto& m : fi.mtx_arrays) {
            if (m.array != 12) continue;     // 12 = XF_A pos matrix array
            if (!g_i60.s_base) g_i60.s_base = m.base;
            seen++;
            for (u32 a : g_i60.blended_addrs)
                if ((a & 0x03FFFFFFu) == (m.base & 0x03FFFFFFu)) { hit++; break; }
        }
        if (!g_i60.blended_addrs.empty()) g_i60.s_addr = g_i60.blended_addrs[0];
        g_i60.redraw_mtx_bases = seen;
        g_i60.redraw_mtx_bases_hit = hit;
    }

    // Undo the blend: restore each model's mDrawMtxBuf[1][view] to tick N.
    interp60_restore_after_redraw();

    MEM_W16(display + 0x4C, wait);   // restore the game's retrace count
    g_i60.redraws++;
}

}  // namespace

// ── /interp60 probe: read live state, set live controls ──────────────────────
// Args (any subset): ?alpha=<float> ?blend=<0|1> ?perturb=<0|1>.
int interp60_probe(char* out, int cap, const char* query) {
    auto farg = [&](const char* key, float def) -> float {
        const char* p = strstr(query, key);
        return p ? (float)atof(p + strlen(key)) : def;
    };
    auto present = [&](const char* key) { return strstr(query, key) != nullptr; };
    if (present("alpha="))   g_i60.alpha   = farg("alpha=", g_i60.alpha);
    if (present("mode="))    g_i60.mode    = (int)farg("mode=", (float)g_i60.mode);
    if (present("blend="))   g_i60.blend   = (int)farg("blend=", (float)g_i60.blend);
    if (present("perturb=")) g_i60.perturb = (int)farg("perturb=", (float)g_i60.perturb);
    if (present("watch=")) {                 // arm the write-watch on the tracked buffer
        extern u32 g_watch_wa; extern bool g_watch_redraw_only;
        const bool on = (int)farg("watch=", 0) != 0;
        g_watch_wa = on ? g_i60.track_cur : 0;
        g_watch_redraw_only = on;
    }

    interp60_take_motion();   // publish + reset motion accumulator

    int n = 0;
    auto app = [&](const char* fmt, auto... a){ if (n < cap) n += snprintf(out+n, cap-n, fmt, a...); };
    const char* mname = g_i60.mode == 0 ? "passthrough" : g_i60.mode == 1 ? "buffer-blend(redraw-viewCalc)" :
                        g_i60.mode == 2 ? "view-blend(camera)" : g_i60.mode == 3 ? "registry-blend(scene-wide)" : "?";
    app("interp60 enabled=%d  mode=%d(%s) alpha=%.3f blend=%d perturb=%d\n",
        (int)sunbright_interp60(), g_i60.mode, mname, g_i60.alpha, g_i60.blend, g_i60.perturb);
    app("CAMERA: view abs=(%.2f,%.2f,%.2f) snaps=%lu\n", g_i60.view_x, g_i60.view_y, g_i60.view_z, g_i60.view_snaps);
    app("CAMERA (mode 2): TJ3DSysSetViewMtx during redraw=%lu  view delta N-1->N=(%.3f,%.3f,%.3f)  %s\n",
        g_i60.setviewmtx_calls, g_i60.view_dx, g_i60.view_dy, g_i60.view_dz,
        g_i60.mode != 2 ? "(mode!=2)" :
        g_i60.setviewmtx_calls == 0 ? "<<< view node NEVER runs in redraw — blend can't reach j3dSys" :
        "(view node runs — blended camera reaches j3dSys)");
    app("redraws=%lu  skips(rate=%lu nodir=%lu full=%lu)  mardir=%08x gfx_valid=%d\n",
        g_i60.redraws, g_i60.skip_rate, g_i60.skip_nodir, g_i60.skip_full,
        g_i60.mardir, g_i60.gfx_valid);
    app("registry(mode3): real-field viewCalc=%lu  models=%lu  blended=%lu bail(null=%lu single=%lu)\n",
        g_i60.vc_realfield, g_i60.reg_size, g_i60.vc_blended, g_i60.vc_bail_null, g_i60.vc_bail_single);
    app("redraw viewCalc calls=%lu\n", g_i60.vc_calls);
    app("  per-list viewCalc:");
    for (u32 i = 0; i < sizeof(kDrawLists)/sizeof(kDrawLists[0]); i++)
        app(" [%u:+%02x]=%lu", i, kDrawLists[i], g_i60.vc_per_list[i]);
    app("\n");
    app("motion (N-1->N msd): n=%lu avg=%.3f min=%.3f max=%.3f\n",
        g_i60.move_n, g_i60.move_avg, g_i60.move_min, g_i60.move_max);
    app("RENDER VOLUME of in-between field: gx_bytes=%llu runs=%lu  %s\n",
        g_i60.redraw_gx_bytes, g_i60.redraw_gx_runs,
        g_i60.redraw_gx_bytes < 1024 ? "<<< ~ZERO: in-between NOT re-rendered (tick N re-presented)" : "(re-rendered)");
    app("BLEND->GPU: pos-mtx array bases set=%lu  of those reading a blended buffer=%lu  %s\n",
        g_i60.redraw_mtx_bases, g_i60.redraw_mtx_bases_hit,
        g_i60.redraw_mtx_bases == 0 ? "(no pos-mtx loads seen)" :
        g_i60.redraw_mtx_bases_hit == 0 ? "<<< GPU reads NONE of our blended buffers" :
        "(GPU reads our blend)");
    app("  sample: first array-12 base=%08x  first blended addr=%08x  (cmp low26: %s)\n",
        g_i60.s_base, g_i60.s_addr,
        (g_i60.s_base & 0x03FFFFFFu) == (g_i60.s_addr & 0x03FFFFFFu) ? "MATCH" : "differ");
    app("WIRING (tracked model %08x view=%u): d1a=%08x cur=%08x  pkt=%08x unk18=%08x unk18[view]=%08x\n",
        g_i60.track_model, g_i60.track_view, g_i60.track_d1a, g_i60.track_cur,
        g_i60.track_pkt, g_i60.track_unk18, g_i60.track_unk18_view);
    app("  -> %s ; %s\n",
        g_i60.track_unk18 == g_i60.track_d1a ? "unk18==mDrawMtxBuf[1] (packet wired to my buffer)"
                                             : "<<< unk18 != mDrawMtxBuf[1] (packet uses a DIFFERENT buffer)",
        g_i60.track_unk18_view == g_i60.track_cur ? "unk18[view]==cur (GPU reads what I blend)"
                                                  : "<<< unk18[view] != cur (GPU reads a different base)");
    app("clobber check (1 model @ %08x): blended=%.3f  after-all-lists=%.3f  %s\n",
        g_i60.track_addr, g_i60.track_blended, g_i60.track_after,
        g_i60.track_addr == 0 ? "(no model tracked)" :
        g_i60.track_blended == g_i60.track_after ? "(blend survived to draw time)" :
        "<<< blend was overwritten before draw");
    return n;
}
