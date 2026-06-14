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
#include "VideoCommon/AbstractGfx.h"   // g_gfx->Flush() between the two fields

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cmath>

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

// Bisection knob: SUNBRIGHT_INTERP60_NOSYNTH=1 keeps INTERP60's viewCalc registration active but
// NEVER synthesizes an in-between field (no blend, no draw-list re-issue, no alt present). Isolates
// "the in-between synthesis crashes" from "the real-field overrides crash".
static bool interp60_nosynth() {
    static int v = -1;
    if (v < 0) v = getenv("SUNBRIGHT_INTERP60_NOSYNTH") ? 1 : 0;
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
size_t interp60_registry_size();        // interp_capture.cpp — live-model count this frame
bool sunbright_interp60_replay();       // interp60_replay.cpp — render-only replay in-between
extern "C" void interp60_xfmap_build(const u8* frame, u32 n, float alpha);  // interp_capture.cpp
extern "C" void interp60_xfmap_set_alpha(float alpha);  // interp_capture.cpp — re-aim map per replay
extern "C" void interp60_xfmap_end();

namespace {

u32 g_mardir = 0;                 // TMarDirector* seen by the last direct()
unsigned long g_direct_stamp = 0, g_consumed_stamp = 0;

extern "C" void func_80299838(CPUState&);   // TMarDirector::direct
// Owned present (interp60): the runtime drives scan-out itself for a deterministic R,B,R,B
// cadence — Dolphin's auto per-field present is gated off (g_sb_own_present), and we call
// sb_present_xfb(phys_addr) exactly twice per game tick (real, in-between). This removes all
// dependence on Dolphin's VI field parity/phase + the progressive even-field offset (RRBB, H5).
extern "C" volatile int g_sb_own_present;       // Present.cpp (fork)
extern "C" void sb_present_xfb(unsigned xfb_addr);   // Present.cpp (fork) — present phys addr now
extern "C" volatile int g_sb_efb_redirect_inbetween;  // BPStructs.cpp (fork) — in-between owned-EFB routing
extern "C" void sb_efb_reset_binds();                 // TextureCacheBase.cpp (fork) — drop bound-tex reuse
extern "C" void sb_clear_efb();                       // BPFunctions.cpp (fork) — clear EFB (own the clear)
static bool efb_own_enabled() {                        // SUNBRIGHT_EFB_OWN: per-field owned EFB textures
    static int v = -1;
    if (v < 0) v = getenv("SUNBRIGHT_EFB_OWN") ? 1 : 0;
    return v == 1;
}
static bool efb_reset_binds_enabled() {                // A/B: SUNBRIGHT_EFB_NORESET disables the fix
    static int v = -1;
    if (v < 0) v = (efb_own_enabled() && !getenv("SUNBRIGHT_EFB_NORESET")) ? 1 : 0;
    return v == 1;
}
static bool own_present_enabled() {
    static int v = -1;
    if (v < 0) v = getenv("SUNBRIGHT_NO_OWN_PRESENT") ? 0 : 1;  // default ON for interp60
    return v == 1;
}
static bool efb_redirect_enabled() {
    static int v = -1;
    if (v < 0) v = getenv("SUNBRIGHT_NO_EFB_REDIRECT") ? 0 : 1;  // default ON for interp60
    return v == 1;
}
extern "C" void sb_efb_native_begin_inbetween();   // efb_native.cpp — per-field EFB-copy redirect
extern "C" void sb_efb_native_end_inbetween();
extern "C" void func_802f80d0(CPUState&);   // TDisplay::endRendering
extern "C" void func_802a4e28(CPUState&);   // TPerformList::perform

// Snapshot of the live TGraphics the game's draw pass used (cameras/viewports
// fill viewport/projection/view there). A fabricated zeroed TGraphics corrupts
// the GX stream, so we copy the real one when the game performs the GX list.
u8  g_gfx_snap[0x100];
bool g_gfx_valid = false;

SUNBRIGHT_OVERRIDE(ov_interp_perform_snap, 0x802a4e28u) {
    if ((sunbright_interp60() || sunbright_interp60_replay()) && g_mardir && cpu.gpr[3] == MEM_R32(g_mardir + 0x1C) && cpu.gpr[5] >= 0x80000000u) {
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

// PROBE (falsified-as-a-fix, kept as a toggle + finding): TEfbCtrlTex::perform (&0x8) does
// GXCopyTex(EFB -> screen texture); SMS water/refraction samples that texture. Skipping the copy
// on the in-between (so it reuses the REAL field's capture) was tried as a flicker fix — but it
// GHOSTS Mario into the reflection (the stale texture has Mario at the real-field position while
// his geometry is at the interpolated position → duplicate in the sky). So default OFF. The
// useful FINDING it proved: the water reflection DOES sample this screen texture, and the
// real flicker is that effects toggle/​differ per field — not a clean reuse problem. Real fix is
// being designed from docs/re_notes/ (which effects draw in which perform list/phase).
extern "C" void func_802f8bac(CPUState&);   // JDrama::TEfbCtrlTex::perform(u32 flags, TGraphics*)
SUNBRIGHT_OVERRIDE(ov_efbctrltex_perform, 0x802f8bacu) {
    if (g_interp60_in_redraw && g_i60.skip_efbcopy) {
        g_i60.efbcopy_skipped++;
        cpu.gpr[4] &= ~0x8u;          // drop the EFB->screen-texture copy; keep setup
    }
    func_802f8bac(cpu);
}

// Instrument the marukage draw (TSilhouette::perform). Split counts by field so we can tell a
// draw-asymmetry blink (fires on one field, not the other) from a position-detach blink (fires
// on both, but its gpMarioPos-projected center is at tick N while the body interpolates).
extern "C" void func_80227914(CPUState&);   // TSilhouette::perform(u32 flags, TGraphics*)
SUNBRIGHT_OVERRIDE(ov_silhouette_perform, 0x80227914u) {
    const u32 param = cpu.gpr[4];
    if (g_interp60_in_redraw) {
        g_i60.sil_perf_redraw++; g_i60.sil_lastparam_redraw = param;
        if (param & 0x10) g_i60.sil_maru_redraw++;
    } else {
        g_i60.sil_perf_real++; g_i60.sil_lastparam_real = param;
        if (param & 0x10) g_i60.sil_maru_real++;
    }
    func_80227914(cpu);
}

SUNBRIGHT_OVERRIDE(ov_interp_mardir_direct, MARDIR_DIRECT) {
    g_mardir = cpu.gpr[3];
    if (g_i60.mode == 3) interp60_registry_clear();   // start a fresh model registry
    func_80299838(cpu);                                // populates it via real-field viewCalc
    g_direct_stamp++;
}

// Engine logic-frame counter (one tick per TDisplay::endRendering) — the OSD readout in
// Dolphin's Present.cpp ViSwap divides this against the present rate to show game-vs-output fps.
// Storage lives in videocommon (Present.cpp) so the offline tools link; we just bump it.
extern "C" volatile unsigned long g_sb_game_seq;

SUNBRIGHT_OVERRIDE(ov_interp_endRendering, DISPLAY_END_RENDER) {
    g_sb_game_seq = g_sb_game_seq + 1;
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
    const bool room = (sunbright_interp60() || sunbright_interp60_replay()) && !interp60_nosynth() && fresh && g_mardir && g_gfx_valid && wait == 2;
    if (!room) {
        if (sunbright_interp60() && fresh && wait != 2) g_i60.skip_rate++;
        if (sunbright_interp60() && fresh && !g_mardir)  g_i60.skip_nodir++;
        if (sunbright_interp60() && fresh && !g_gfx_valid) g_i60.skip_full++;
        // No in-between this frame: hand presentation back to Dolphin's automatic VI path.
        g_sb_own_present = 0;
        func_802f80d0(cpu);
        return;
    }


    // SMS is single-XFB-buffered (display's two buffer slots are identical), so the real
    // frame and the in-between would copy to and present from the SAME XFB texture — the
    // async VI then shows it twice = 30 fps (RE'd via the present-address ring). Give the
    // in-between its OWN distinct XFB address so the two presents alternate (true 60 fps).
    // copy_to_ram is false (XFBToTexture), so a second address writes NO guest RAM — it is
    // purely a separate texture-cache key.
    const u32 orig_fb = MEM_R32(display + 4);
    const u32 alt = orig_fb ^ 0x00400000u;

    // ── Render-only replay in-between (SUNBRIGHT_INTERP60_REPLAY) ─────────────────────────────────
    // interp60 ON TOP of the game: present the real frame, then render the in-between by REPLAYING
    // frame N's captured GX command stream through the OpcodeDecoder — no game code runs, no game
    // memory is written (the old mutate-mDrawMtxBuf + re-issue-perform-lists path below is bypassed).
    // This eliminates the whole crash class (particle/shape/director re-execution). TODO: per-object
    // matrix interpolation toward N-1 via GXSetArray base redirection; currently replays N as-is, so
    // the in-between == N (true 60fps cadence, but no motion-interpolation yet).
    // Fires for EITHER flag: SUNBRIGHT_INTERP60 (run60) now uses the render-only replay, so the old
    // mutating path below is unreachable (kept transiently for A/B; to be deleted).
    if (sunbright_interp60() || sunbright_interp60_replay()) {
        // ── OWN BOTH PRESENTS through the SAME render path (one outcome by construction) ──
        // The game simulates at 30 Hz and renders frame N once through the live FIFO (that render
        // built the captured command stream below; its EFB output is discarded). We then re-render
        // BOTH presented frames OURSELVES from that one stream, each into a freshly-cleared EFB:
        //   REAL       = replay @ alpha 1.0  (== frame N exactly)
        //   IN-BETWEEN = replay @ g_i60.alpha (lerp N-1 -> N)
        // Each replay re-runs the engine's OWN EFB-copy / screen-texture commands, so each present
        // samples its OWN freshly-copied screen texture. This is what fixes Frontier 2 (effects
        // wrong on REAL frames): previously the real present was the game's LIVE render, which
        // sampled the screen texture left by the PREVIOUS in-between (N-1/2) => water/mirror/EFB
        // feedback lagged a half-step on real frames, while the in-between (a replay) looked right.
        // Real and in-between were two DIFFERENT pipelines and disagreed. Now they are one pipeline
        // and cannot disagree. A/B off-switch: SUNBRIGHT_NO_UNIFIED_REPLAY (old live-real path).
        //
        // OWN the present (default): the runtime scans out both frames itself (sb_present_xfb) for a
        // deterministic R,B,R,B cadence, independent of Dolphin's VI field parity/phase (hazard H5).
        // Addresses are physical (GXCopyDisp dest form, & 0x3FFFFFFF) — the XFB cache key.
        const bool own = own_present_enabled();
        const u32 orig_phys = orig_fb & 0x3FFFFFFFu;
        const u32 alt_phys  = alt     & 0x3FFFFFFFu;
        if (own) g_sb_own_present = 1;
        const u32 video = MEM_R32(display + 0x60);
        g_i60.disp = display; g_i60.set_fb = alt;

        // Snapshot frame N's captured command stream NOW — the copies below do GXCopyDisp which
        // triggers gxs_frame_boundary and clears g_frame. frameN is a stable private copy.
        std::vector<u8> frameN(gxs_cur_frame());
        g_i60.redraw_gx_bytes = frameN.size();

        // Build the pos-matrix pairing map ONCE, with correct freshly-spawned gating (against LAST
        // frame's registry). The map is alpha-independent base<->base pairing; we re-aim it at each
        // alpha below without rebuilding (interp60_xfmap_set_alpha). Reads model RAM buffers (stable
        // across both replays — the replay writes only XF/GPU, never guest RAM).
        interp60_xfmap_build(frameN.data(), (u32)frameN.size(), g_i60.alpha);

        const bool unified = getenv("SUNBRIGHT_NO_UNIFIED_REPLAY") == nullptr;

        if (unified) {
            // ── REAL present: replay frame N (alpha 1.0) into a clean EFB, copy -> orig, present ──
            sb_clear_efb();                                    // discard the game's live EFB; render fresh
            interp60_xfmap_set_alpha(1.0f);
            g_interp60_in_redraw = true;
            gxs_replay_frame(frameN.data(), frameN.size());    // re-render N (own EFB-copy/screen tex)
            g_interp60_in_redraw = false;
            MEM_W16(display + 0x4C, 1);
            cpu.gpr[3] = display; func_802f80d0(cpu);          // pace 1 field + copy real EFB -> orig
            if (g_gfx) g_gfx->Flush();
            if (own) sb_present_xfb(orig_phys);                // R: present the real frame now

            // ── IN-BETWEEN present: replay frame N (g_i60.alpha) into a clean EFB, copy -> alt ──
            cpu.gpr[3] = video; cpu.gpr[4] = alt; call_ppc(cpu, VIDEO_SET_NEXT_XFB);
            sb_clear_efb();
            interp60_xfmap_set_alpha(g_i60.alpha);
            g_interp60_in_redraw = true;
            gxs_replay_frame(frameN.data(), frameN.size());    // re-render N-1/2
            g_interp60_in_redraw = false;
            interp60_xfmap_end();
            const u32 ob0 = MEM_R32(display + 4), ob1 = MEM_R32(display + 8);
            MEM_W32(display + 4, alt); MEM_W32(display + 8, alt);   // steer the copy dest to ALT
            MEM_W16(display + 0x4C, 1);
            cpu.gpr[3] = display; func_802f80d0(cpu);          // pace 1 field + copy in-between -> alt
            MEM_W32(display + 4, ob0); MEM_W32(display + 8, ob1);
            MEM_W16(display + 0x4C, wait);
            if (own) sb_present_xfb(alt_phys);                 // B: present the in-between now
            g_i60.redraws++;
            return;
        }

        // ── A/B fallback (SUNBRIGHT_NO_UNIFIED_REPLAY): old live-real-frame path ──
        // Real present is the game's LIVE render (sampled the previous in-between's screen texture =
        // the half-step effect lag this whole change fixes). Kept only for A/B until the user
        // confirms the unified path headed; then delete.
        MEM_W16(display + 0x4C, 1);
        cpu.gpr[3] = display; func_802f80d0(cpu);          // pace 1 field + copy REAL frame N → orig
        if (g_gfx) g_gfx->Flush();
        if (own) sb_present_xfb(orig_phys);                // R: present the real frame now
        cpu.gpr[3] = video; cpu.gpr[4] = alt; call_ppc(cpu, VIDEO_SET_NEXT_XFB);   // in-between → ALT
        g_interp60_in_redraw = true;
        if (efb_redirect_enabled()) g_sb_efb_redirect_inbetween = 1;
        if (efb_reset_binds_enabled()) sb_efb_reset_binds();
        gxs_replay_frame(frameN.data(), frameN.size());    // re-render frame N with interpolated mtx
        g_sb_efb_redirect_inbetween = 0;
        if (efb_reset_binds_enabled()) sb_efb_reset_binds();
        interp60_xfmap_end();
        g_interp60_in_redraw = false;
        const u32 ob0 = MEM_R32(display + 4), ob1 = MEM_R32(display + 8);
        MEM_W32(display + 4, alt); MEM_W32(display + 8, alt);   // steer the copy dest to ALT
        MEM_W16(display + 0x4C, 1);
        cpu.gpr[3] = display; func_802f80d0(cpu);          // pace 1 field + copy replayed EFB → ALT
        MEM_W32(display + 4, ob0); MEM_W32(display + 8, ob1);
        MEM_W16(display + 0x4C, wait);
        if (own) sb_present_xfb(alt_phys);                 // B: present the in-between now
        g_i60.redraws++;
        return;
    }   // distinct low26, 32-aligned, within MEM1

    // First half: one field + the real frame's copy (EFB → XFB at the game's address).
    MEM_W16(display + 0x4C, 1);
    cpu.gpr[3] = display;
    func_802f80d0(cpu);

    // Submit the real field's command buffer NOW, before the in-between re-issues a second
    // full scene. The two fields would otherwise pile both scenes' vertices into the GPU
    // streaming buffer with no intervening submit to recycle it — it overflows and the vertex
    // manager force-flushes mid-draw ("Executing command list while waiting for space…"),
    // stalling on a fence and tanking the framerate. A proactive Flush (Vulkan: submit
    // off-thread, no present; OGL: no-op) recycles the buffer between the two frames. It is
    // also semantically right: the real field IS a finished frame.
    if (g_gfx) g_gfx->Flush();

    // Tell the VI the in-between present will land at ALT (the present address comes from
    // this setNextXFB, NOT from display's fb pointer — verified: redirecting only the copy
    // left the present on the original buffer).
    const u32 video = MEM_R32(display + 0x60);
    cpu.gpr[3] = video;
    cpu.gpr[4] = alt;
    call_ppc(cpu, VIDEO_SET_NEXT_XFB);
    g_i60.disp = display; g_i60.buf0 = MEM_R32(display + 4); g_i60.buf1 = MEM_R32(display + 8); g_i60.set_fb = alt;

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
        g_i60.is_cut = 0;
        if ((g_i60.mode == 2 || g_i60.mode == 3) && g_i60.have_prev_view) {
            const float a = g_i60.alpha;
            g_i60.view_dx = cur_view[3]  - prev_view[3];
            g_i60.view_dy = cur_view[7]  - prev_view[7];
            g_i60.view_dz = cur_view[11] - prev_view[11];
            // Camera-level cut detector: a scene cut jumps the view far in one frame.
            // Skip the whole in-between blend then (N-1 and N are different scenes) —
            // far better than a per-object magnitude guard, which misfires on the
            // large but smooth eye-space motion of distant geometry during rotation.
            const float mag = std::sqrt(g_i60.view_dx*g_i60.view_dx + g_i60.view_dy*g_i60.view_dy + g_i60.view_dz*g_i60.view_dz);
            if (mag > 8000.0f) { g_i60.is_cut = 1; g_i60.cuts++; }
            if (!g_i60.is_cut) for (u32 i = 0; i < 12; i++) {
                float v = (1.0f - a) * prev_view[i] + a * cur_view[i];
                if (g_i60.perturb && (i == 3)) v = cur_view[i] + 300.0f;  // unmissable A/B
                mem_wf32(J3DSYS_VIEWMTX + i * 4, v);   // j3dSys directly
                mem_wf32(gfx + 0xB4 + i * 4, v);       // and the gfx the view node reads
            }
        }
        for (u32 i = 0; i < 12; i++) prev_view[i] = cur_view[i];
        g_i60.have_prev_view = 1;
    }

    // ── Mario's world position for projected effects (shadow + water-surface shadow) ──
    // TSilhouette::perform (the +0x20 silhouette list) projects Mario's drop shadow onto the
    // ground AND the water using the LIVE global gpMarioPos (-0x60B4 off the SDA base r13),
    // NOT a draw matrix — so the registry blend never reaches it. On the in-between field the
    // shadow snapped to tick-N gpMarioPos while Mario's model interpolated, the two detaching
    // every other frame: the in-sync water+shadow 30 Hz flicker (RE'd from
    // reference/sms MarioUtil/DrawUtil.cpp TSilhouette::perform/setting). Fix: blend the global
    // Vec toward N-1 for the duration of the redraw so every gpMarioPos reader (shadow, any
    // water 2D overlay) sees the same interpolated position the geometry uses; restore after.
    u32 mario_vec = 0; float saved_mario[3] = {0,0,0};
    {
        const u32 pp = cpu.gpr[13] + (u32)(s32)(s16)0x9f4c;   // &gpMarioPos
        const u32 v  = (pp >= 0x80000000u && pp < 0x81800000u) ? MEM_R32(pp) : 0;
        g_i60.mario_vec = v;
        if (v >= 0x80000000u && v < 0x81800000u) {
            mario_vec = v;
            static float prev_mario[3]; static int have_prev_mario = 0;
            float cur_mario[3];
            for (u32 i = 0; i < 3; i++) cur_mario[i] = mem_rf32(v + i * 4);
            for (u32 i = 0; i < 3; i++) saved_mario[i] = cur_mario[i];   // restore after redraw
            g_i60.mario_x = cur_mario[0]; g_i60.mario_y = cur_mario[1]; g_i60.mario_z = cur_mario[2];
            if (g_i60.shadow_blend && (g_i60.mode == 2 || g_i60.mode == 3) && have_prev_mario && !g_i60.is_cut) {
                const float a = g_i60.alpha;
                g_i60.mario_dx = cur_mario[0] - prev_mario[0];
                g_i60.mario_dy = cur_mario[1] - prev_mario[1];
                g_i60.mario_dz = cur_mario[2] - prev_mario[2];
                for (u32 i = 0; i < 3; i++)
                    mem_wf32(v + i * 4, (1.0f - a) * prev_mario[i] + a * cur_mario[i]);
            }
            for (u32 i = 0; i < 3; i++) prev_mario[i] = cur_mario[i];
            have_prev_mario = 1;
        }
    }

    // Silhouette-manager state probe (confirm the in-between double-mutates occlusion alpha).
    u32 sil_mgr = 0;
    {
        const u32 pp = cpu.gpr[13] + (u32)(s32)(s16)0x9f6c;   // &gpSilhouetteManager
        const u32 m  = (pp >= 0x80000000u && pp < 0x81800000u) ? MEM_R32(pp) : 0;
        if (m >= 0x80000000u && m < 0x81800000u) { sil_mgr = m; g_i60.sil_mgr = m; g_i60.sil_before = mem_rf32(m + 0x48); }
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
    // Blend the whole scene's registered models (J3DModel + SDLModel/world) toward
    // N-1, unless this is a scene cut. Both classes double-buffer mDrawMtxBuf, so
    // blend_model handles them uniformly; their viewCalc bodies are skipped in the
    // redraw so the blend survives the re-issue.
    static const bool noblend = getenv("SUNBRIGHT_INTERP60_NOBLEND") != nullptr;  // bisect: redraw w/o blend
    if (g_i60.mode == 3 && !g_i60.is_cut && !noblend) interp60_blend_registry();
    // Native per-field EFB-copy: redirect this in-between's EFB-copy resolves + their consumer
    // texture reads to a distinct ALT address (efb_native.cpp) so the real field's screen/mirror/
    // graffiti textures are not overwritten — each field gets its own correct copy = true 60fps
    // screen-space effects, no flicker, no ghost. The tracked set was populated by THIS pair's real
    // field (ov_efb_native_copytex self-tracks every real-field GXCopyTex dest).
    sb_efb_native_begin_inbetween();
    for (u32 li = 0; li < sizeof(kDrawLists) / sizeof(kDrawLists[0]); li++) {
        if (!(g_i60.list_mask & (1u << li))) continue;   // bisection: skip masked-off passes
        const u32 list = MEM_R32(g_mardir + kDrawLists[li]);
        if (!list) continue;
        g_i60.cur_list = (int)li;
        cpu.gpr[3] = list;
        // The in-between must DRAW, not advance state. Clearing &1 (movement) + &2 (calc-anim)
        // stops re-running the water move()/scroll counter (unk5E00) and other per-frame anim
        // that double-steps the texture every other field = the water flicker (RE:
        // docs/re_notes/water_60fps_correctness.md). Keeps view-calc/draw/post (0x4/0x8/0x10/0x80).
        // Live-tunable for A/B: /interp60?perform_mask=0xffffffff restores the old all-bits pass.
        cpu.gpr[4] = g_i60.perform_mask;
        cpu.gpr[5] = gfx;
        call_ppc(cpu, PERFORM_LIST_PERFORM);
    }
    g_i60.cur_list = -1;
    // (The cast shadow now draws IN PHASE during the +0x20 silhouette list re-issue above —
    // see shadow_interp.cpp. The old trailing replay ran after +0x24's EFB->XFB copy = out of
    // phase = the blink, and is removed.)
    call_ppc(cpu, GX_INVALIDATE_TEXALL);
    sb_efb_native_end_inbetween();
    if (sil_mgr) g_i60.sil_after = mem_rf32(sil_mgr + 0x48);   // did the in-between move unk48?
    g_interp60_in_redraw = false;
    cpu.gpr[1] = saved_r1;

    // Restore j3dSys's view matrix to tick N (we blended it for the redraw).
    if ((g_i60.mode == 2 || g_i60.mode == 3) && g_i60.have_prev_view)
        for (u32 i = 0; i < 12; i++) mem_wf32(J3DSYS_VIEWMTX + i * 4, saved_jview[i]);

    // Restore gpMarioPos to tick N (we blended it for the shadow projection).
    if (mario_vec) for (u32 i = 0; i < 3; i++) mem_wf32(mario_vec + i * 4, saved_mario[i]);

    // Clobber check: is the blend we wrote still in RAM now that all draw lists ran?
    if (g_i60.track_addr) g_i60.track_after = mem_rf32(g_i60.track_addr + 3 * 4);

    // Second half: one more field + the redraw's copy. Steer the display's fb pointer to
    // ALT so endRendering's GXCopyDisp writes the blend into texture[ALT] (matching the
    // setNextXFB(alt) above, so the present reads what we copied). Restore afterward.
    const u32 orig_b0 = MEM_R32(display + 4), orig_b1 = MEM_R32(display + 8);
    MEM_W32(display + 4, alt);
    MEM_W32(display + 8, alt);
    MEM_W16(display + 0x4C, 1);
    cpu.gpr[3] = display;
    func_802f80d0(cpu);
    MEM_W32(display + 4, orig_b0);            // restore the game's single XFB
    MEM_W32(display + 8, orig_b1);

    // GX volume + decode-run delta for the whole in-between field.
    g_i60.redraw_gx_bytes = gxs_decoded_bytes() - bytes0;
    g_i60.redraw_gx_runs  = gxs_decode_runs()  - runs0;

    // DECISIVE cross-check: the redraw frame's copy just rotated g_prev_info.
    // Count how many POS-matrix array bases (CP array 12) the GX stream set point
    // at a buffer we blended — i.e. how much of the GPU's matrix input is ours.
    {
        const GxFrameInfo& fi = gxs_prev_frame_info();
        unsigned long seen = 0, hit = 0;
        // GXSetArray stores a PHYSICAL base (virtual & 0x03FFFFFF); blended_addrs
        // are virtual — compare on the low 26 bits.
        g_i60.s_base = g_i60.s_addr = 0;
        g_i60.miss_n = 0;
        for (const auto& m : fi.mtx_arrays) {
            if (m.array != 12) continue;     // 12 = XF_A pos matrix array
            if (!g_i60.s_base) g_i60.s_base = m.base;
            seen++;
            bool found = false;
            for (u32 a : g_i60.blended_addrs)
                if ((a & 0x03FFFFFFu) == (m.base & 0x03FFFFFFu)) { hit++; found = true; break; }
            if (!found && g_i60.miss_n < 8) g_i60.miss_base[g_i60.miss_n++] = (m.base & 0x03FFFFFFu) | 0x80000000u;
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
    if (present("shadow="))  g_i60.shadow_blend = (int)farg("shadow=", (float)g_i60.shadow_blend);
    if (present("skip_efbcopy=")) g_i60.skip_efbcopy = (int)farg("skip_efbcopy=", (float)g_i60.skip_efbcopy);
    if (present("listmask=")) {              // hex bitmask of kDrawLists indices to re-issue
        const char* p = strstr(query, "listmask=");
        g_i60.list_mask = (unsigned)strtoul(p + 9, nullptr, 0);
    }
    if (present("perform_mask=")) {          // perform() flag mask for the in-between re-issue
        const char* p = strstr(query, "perform_mask=");
        g_i60.perform_mask = (unsigned)strtoul(p + 13, nullptr, 0);
    }
    if (present("watch=")) {                 // arm the write-watch on the tracked buffer
        extern u32 g_watch_wa; extern bool g_watch_redraw_only;
        const bool on = (int)farg("watch=", 0) != 0;
        g_watch_wa = on ? g_i60.track_cur : 0;
        g_watch_redraw_only = on;
    }
    if (present("watchaddr=")) {              // arm on an arbitrary addr, ALL writers (not redraw-gated)
        extern u32 g_watch_wa; extern bool g_watch_redraw_only;
        const char* p = strstr(query, "watchaddr="); u32 a = (u32)strtoul(p + 10, nullptr, 16);
        g_watch_wa = a; g_watch_redraw_only = false;
    }

    interp60_take_motion();   // publish + reset motion accumulator

    int n = 0;
    auto app = [&](const char* fmt, auto... a){ if (n < cap) n += snprintf(out+n, cap-n, fmt, a...); };
    const char* mname = g_i60.mode == 0 ? "passthrough" : g_i60.mode == 1 ? "buffer-blend(redraw-viewCalc)" :
                        g_i60.mode == 2 ? "view-blend(camera)" : g_i60.mode == 3 ? "registry-blend(scene-wide)" : "?";
    app("interp60 enabled=%d  mode=%d(%s) alpha=%.3f blend=%d perturb=%d\n",
        (int)sunbright_interp60(), g_i60.mode, mname, g_i60.alpha, g_i60.blend, g_i60.perturb);
    app("CAMERA: view abs=(%.2f,%.2f,%.2f) snaps=%lu\n", g_i60.view_x, g_i60.view_y, g_i60.view_z, g_i60.view_snaps);
    app("CAMERA (view-blend, modes 2&3): TJ3DSysSetViewMtx during redraw=%lu  view delta N-1->N=(%.3f,%.3f,%.3f)  %s\n",
        g_i60.setviewmtx_calls, g_i60.view_dx, g_i60.view_dy, g_i60.view_dz,
        (g_i60.mode != 2 && g_i60.mode != 3) ? "(view-blend off)" :
        g_i60.setviewmtx_calls == 0 ? "<<< view node NEVER runs in redraw — blend can't reach j3dSys" :
        "(view node runs — blended camera reaches j3dSys -> world recompute)");
    app("redraws=%lu  skips(rate=%lu nodir=%lu full=%lu)  mardir=%08x gfx_valid=%d\n",
        g_i60.redraws, g_i60.skip_rate, g_i60.skip_nodir, g_i60.skip_full,
        g_i60.mardir, g_i60.gfx_valid);
    app("MOTION-INTERP (LoadIndexedXF hook): map=%lu objects  hits=%lu (pos-mtx loads interpolated) "
        "misses=%lu  %s\n", g_i60.xf_map_size, g_i60.xf_hits, g_i60.xf_misses,
        g_i60.xf_hits ? "<<< INTERPOLATING" : "<<< NOT interpolating (no paired objects)");
    {   // indexed-XF array histogram: 12=pos(interpolated); others (texgen/tex-mtx) are NOT yet.
        extern unsigned long g_xf_array_hist[16];
        app("  XF indexed-load arrays (per-frame class):");
        for (int i = 0; i < 16; i++) if (g_xf_array_hist[i]) app(" [%d]=%lu", i, g_xf_array_hist[i]);
        app("\n");
    }
    app("registry(mode3): real-field viewCalc=%lu  models=%lu  blended=%lu bail(null=%lu single=%lu)\n",
        g_i60.vc_realfield, g_i60.reg_size, g_i60.vc_blended, g_i60.vc_bail_null, g_i60.vc_bail_single);
    app("redraw viewCalc calls=%lu\n", g_i60.vc_calls);
    app("cut detect: is_cut=%d cuts=%lu  (world now via SDLModel registry, not base-keyed)\n",
        g_i60.is_cut, g_i60.cuts);
    app("SHADOW (gpMarioPos blend): shadow_blend=%d vec=%08x pos=(%.1f,%.1f,%.1f) deltaN-1->N=(%.2f,%.2f,%.2f)  %s\n",
        g_i60.shadow_blend, g_i60.mario_vec, g_i60.mario_x, g_i60.mario_y, g_i60.mario_z,
        g_i60.mario_dx, g_i60.mario_dy, g_i60.mario_dz,
        g_i60.mario_vec ? "(resolved)" : "<<< gpMarioPos UNRESOLVED (shadow blend no-op)");
    app("list_mask=0x%02x (bits=re-issued lists: 0:+40 1:+38 2:+3c 3:+1c[GX] 4:+20[silhouette] 5:+24[GXPost])\n",
        g_i60.list_mask);
    app("SILHOUETTE mgr=%08x unk48(occlusion alpha) before=%.4f after-inbetween=%.4f  %s\n",
        g_i60.sil_mgr, g_i60.sil_before, g_i60.sil_after,
        g_i60.sil_mgr == 0 ? "<<< unresolved" :
        (g_i60.sil_after != g_i60.sil_before) ? "<<< in-between MUTATED occlusion alpha (blink source)" :
        "(in-between left it unchanged)");
    app("EFB-COPY skip (water/screen-space flicker fix): skip_efbcopy=%d  copies_skipped_on_inbetween=%lu\n",
        g_i60.skip_efbcopy, g_i60.efbcopy_skipped);
    app("MARUKAGE (TSilhouette::perform): real perf=%lu maru(&0x10)=%lu param=%08x | redraw perf=%lu maru=%lu param=%08x  %s\n",
        g_i60.sil_perf_real, g_i60.sil_maru_real, g_i60.sil_lastparam_real,
        g_i60.sil_perf_redraw, g_i60.sil_maru_redraw, g_i60.sil_lastparam_redraw,
        (g_i60.sil_maru_redraw == 0 && g_i60.sil_maru_real > 0) ? "<<< marukage draws on REAL but NOT in-between (on/off blink)" :
        (g_i60.sil_maru_real == 0 && g_i60.sil_maru_redraw > 0) ? "<<< marukage draws on in-between but NOT real (on/off blink)" :
        "(fires on both fields -> position-detach, not on/off)");
    app("  per-list viewCalc:");
    for (u32 i = 0; i < sizeof(kDrawLists)/sizeof(kDrawLists[0]); i++)
        app(" [%u:+%02x]=%lu", i, kDrawLists[i], g_i60.vc_per_list[i]);
    app("\n");
    app("motion (N-1->N msd): n=%lu avg=%.3f min=%.3f max=%.3f\n",
        g_i60.move_n, g_i60.move_avg, g_i60.move_min, g_i60.move_max);
    app("  WORST offender: model=%08x vt=%08x src=%s view=%u n=%u prevbuf=%08x curbuf=%08x prev0=%g cur0=%g\n",
        g_i60.g_model, g_i60.g_vt, g_i60.g_src == 2 ? "SDLModel" : g_i60.g_src == 1 ? "J3DModel" : "?",
        g_i60.g_view, g_i60.g_n, g_i60.g_prevbuf, g_i60.g_curbuf, g_i60.g_prev0, g_i60.g_cur0);
    // Ground truth: the actual XFB addresses the presenter showed (last 8 presents).
    // If they alternate between two distinct buffers, 2 distinct frames/game-frame reach
    // the screen (60fps). If the same address repeats, the in-between isn't presented.
    {
        extern volatile unsigned long g_sb_present_seq;
        extern volatile unsigned int g_sb_present_ring[16];
        extern volatile unsigned char g_sb_present_dup[16];
        unsigned long s = g_sb_present_seq;
        app("XFB BUFS: display=%08x buf0=%08x buf1=%08x  set_fb(redraw)=%08x  (buf0==buf1? %s)\n",
            g_i60.disp, g_i60.buf0, g_i60.buf1, g_i60.set_fb, g_i60.buf0 == g_i60.buf1 ? "YES single-buffered" : "no");
        app("PRESENTED XFB addrs (newest last):");
        for (int i = 8; i >= 1; i--) {
            unsigned long idx = s - (unsigned long)i;
            if ((long)(s) - i < 0) continue;
            app(" %08x%s", g_sb_present_ring[idx & 15], g_sb_present_dup[idx & 15] ? "(dup)" : "");
        }
        app("\n");
        // Cheap always-on cadence (no readback, unlike /verify): fraction of unique presents that
        // DOUBLED (two presents to the same buffer in a row = a 60Hz hitch). 0% = clean R,B,R,B.
        extern volatile unsigned long g_sb_cadence_alt, g_sb_cadence_dbl;
        const unsigned long alt = g_sb_cadence_alt, dbl = g_sb_cadence_dbl, tot = alt + dbl;
        app("CADENCE (lifetime, no readback): alt=%lu doublings=%lu  doubling-rate=%.1f%%  (0%% = clean 60fps R,B,R,B)\n",
            alt, dbl, tot ? 100.0 * dbl / tot : 0.0);
        extern volatile unsigned long g_sb_ownpres_manual, g_sb_ownpres_gated, g_sb_ownpres_auto;
        extern volatile unsigned g_sb_ownpres_last;
        app("OWN-PRESENT: active=%d  manual=%lu gated-fields=%lu auto-presents=%lu  last_manual=%08x\n",
            g_sb_own_present, g_sb_ownpres_manual, g_sb_ownpres_gated, g_sb_ownpres_auto, g_sb_ownpres_last);
        extern volatile unsigned long g_sb_efb_redirects;
        extern volatile unsigned long g_sb_efb_owned_hits, g_sb_efb_owned_miss, g_sb_efb_inbetween_ramwrite;
        extern volatile unsigned long g_sb_efb_resets, g_sb_efb_real_reused_owned;
        app("EFB-OWNED: copies=%lu  in-between samples hit=%lu miss=%lu  fallthrough-RAM=%lu\n",
            g_sb_efb_redirects, g_sb_efb_owned_hits, g_sb_efb_owned_miss, g_sb_efb_inbetween_ramwrite);
        app("EFB-LEAK: bind-resets=%lu  REAL-frame reused OWNED tex=%lu  %s\n",
            g_sb_efb_resets, g_sb_efb_real_reused_owned,
            g_sb_efb_real_reused_owned ? "<<< LEAK: real frames sample the in-between's texture (= lag)"
                                       : "(no owned-texture leak into real frames)");
    }
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
    app("  UN-blended bases (GPU read, not ours) [virt]:");
    for (unsigned i = 0; i < g_i60.miss_n; i++) app(" %08x", g_i60.miss_base[i]);
    app("\n");
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
