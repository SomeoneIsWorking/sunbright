#pragma once
// Single source of truth for the 60 fps interpolation feature.
//
// SUNBRIGHT_INTERP60=1 turns it on. Everything 60-fps-related gates on this one
// function — there are deliberately no sub-flags (no per-list bisect mask, no
// blend on/off). Defined in runtime/overrides/interp_redraw.cpp.
bool sunbright_interp60();

#include "cpu_state.h"
#include <vector>

// ── Live debug + control state, driven over HTTP at /interp60 ────────────────
// Shared between interp_redraw.cpp (the in-between-field driver) and
// interp_capture.cpp (the J3DModel::viewCalc blend). The probe server
// (runtime/probe_server.cpp) reads the observations and writes the controls, so
// the whole 60 fps data path can be interrogated and A/B'd against a LIVE
// headless run with no rebuild/env cycle. See interp60_probe().
struct Interp60Dbg {
    // ── controls (settable live: /interp60?alpha=..&mode=..&perturb=..) ──
    float alpha   = 0.5f;  // blend toward N: 0 = tick N-1, 1 = tick N. 0.5 = midpoint.
    int   mode    = 3;     // 0 = passthrough (present N twice, no interp)
                           // 1 = buffer-blend in the redraw's viewCalc (reaches only the
                           //     ~14 models whose viewCalc fires DURING the draw pass)
                           // 2 = view-blend: interpolate the CAMERA (j3dSys view matrix)
                           //     and let viewCalc recompute — reaches only those ~14 too
                           // 3 = registry buffer-blend: record EVERY model from the real
                           //     field's viewCalc (runs for all models), then blend each
                           //     one's draw-matrix double-buffer on the in-between field —
                           //     reaches the whole scene (object + camera). THE FIX.
    int   blend   = 1;     // (mode 1 only) 0 = re-present tick N unblended
    int   perturb = 0;     // 1 = GROSS offset instead of blend (perturbation test):
                           //     mode 1 = +offset draw mtx; mode 2 = +offset the camera
    int   shadow_blend = 0;// 1 = interpolate gpMarioPos for the in-between (projected shadow);
                           //     off by default (it made the marukage blink faster, not better).
    unsigned list_mask = 0x3F;  // which kDrawLists[] indices to RE-ISSUE on the in-between field
                           //     (bit i = list i). Bisection knob for the shadow/water blink:
                           //     clear a bit to stop re-drawing that pass on the in-between.
    unsigned perform_mask = 0xFFFFFFFCu;  // perform() flags for the in-between re-issue. Default
                           //     clears &1 (movement) + &2 (calc-anim) so the in-between only
                           //     DRAWS and never double-steps animation/water-scroll (the flicker).
                           //     /interp60?perform_mask=0xffffffff = old all-bits pass (A/B).

    // ── observations (engine writes, probe reads) ──
    // redraw insertion
    unsigned long redraws = 0, skip_rate = 0, skip_nodir = 0, skip_full = 0;
    unsigned long direct_stamp = 0;
    u32 mardir = 0; int gfx_valid = 0;
    // XFB double-buffer wiring (the 30fps-output probe): the TDisplay and its two XFB
    // buffer pointers, the address we set the VI to for the in-between present, and the
    // physical address the VI actually presented (cmp low26 against the buffers).
    u32 disp = 0, buf0 = 0, buf1 = 0, set_fb = 0;
    // viewCalc blend (interp_capture.cpp), last window
    unsigned long vc_calls = 0, vc_blended = 0, vc_bail_null = 0, vc_bail_single = 0;
    unsigned long vc_per_list[8] = {0};   // viewCalc attributed to each kDrawLists[] index
    int cur_list = -1;                    // list index currently executing in the redraw
    // motion magnitude between tick N-1 and N (mean squared translation delta)
    double move_min = 0, move_max = 0, move_avg = 0; unsigned long move_n = 0;
    // worst-offender capture (the model with the largest moved this window): identify
    // WHY prev is garbage. vt=model's vtable (-> class), src=1 J3DModel/2 SDLModel.
    u32 g_model = 0, g_vt = 0, g_view = 0, g_n = 0, g_prevbuf = 0, g_curbuf = 0; int g_src = 0;
    float g_prev0 = 0, g_cur0 = 0; double g_worst = 0;

    // ── DECISIVE: does the blend reach the GPU? ──
    // Every mDrawMtxBuf[1][view] address the blend wrote during the LAST redraw.
    std::vector<u32> blended_addrs;
    // POS-matrix array bases (CP array 12) the GX stream SET during the redraw
    // frame = the addresses the GPU's indexed matrix load actually reads from.
    unsigned long redraw_mtx_bases = 0;       // count seen in the redraw frame
    unsigned long redraw_mtx_bases_hit = 0;   // of those, how many are a blended addr
    // GX volume actually decoded during the redraw (≈0 ⇒ the in-between field never
    // re-rendered; it is just tick N's XFB re-presented — the no-motion smoking gun).
    unsigned long long redraw_gx_bytes = 0;
    unsigned long      redraw_gx_runs = 0;
    u32 s_base = 0, s_addr = 0;   // sample: first array-12 base / first blended addr
    u32 miss_base[8] = {0};       // sample of pos-mtx bases the GPU read that we did NOT blend
    unsigned miss_n = 0;

    // OUTPUT-SIDE proof: hash the real-field XFB vs the in-between-field XFB. If
    // they are ever DIFFERENT, the in-between produced different pixels (motion or
    // perturb reached the screen). If always identical, the in-between is a copy.
    u32 xfb_real_hash = 0, xfb_redraw_hash = 0;
    unsigned long xfb_pairs = 0, xfb_differ = 0;   // how often they differed

    // ── clobber check: is the blend still in RAM at GX-draw time? ──
    // One tracked model: its blended translation, then re-read after all draw lists.
    u32 track_addr = 0; float track_after = 0, track_blended = 0;

    // ── model→packet→base wiring of the tracked model (the disconnect probe) ──
    // model, mDrawMtxBuf[1] (d1a), cur=d1a[view], the shape packet's unk18, and
    // unk18[view] (what setModelDrawMtx feeds GXSetArray). cur should == unk18[view].
    u32 track_model = 0, track_d1a = 0, track_cur = 0;
    u32 track_pkt = 0, track_unk18 = 0, track_unk18_view = 0, track_view = 0;

    // ── view-blend (mode 2) instrumentation ──
    unsigned long vc_realfield = 0;       // viewCalc calls on REAL fields (≈ all models)
    unsigned long reg_size = 0;           // models recorded in the registry last frame
    unsigned long setviewmtx_calls = 0;   // TJ3DSysSetViewMtx::perform during redraw
    float view_dx = 0, view_dy = 0, view_dz = 0;  // camera translation delta N-1 -> N
    float view_x = 0, view_y = 0, view_z = 0;     // absolute view translation (sanity)
    unsigned long view_snaps = 0;                 // # frames a view was read at gfx+0xB4
    int   have_prev_view = 0;
    int   is_cut = 0;                             // view jumped this frame -> skip the in-between blend
    unsigned long cuts = 0;                       // cut count (diagnostic)
    unsigned long base_blended = 0;              // world bases blended last redraw (diagnostic)
    // gpMarioPos shadow-projection interpolation (interp_redraw.cpp): the resolved Vec*,
    // tick-N position, and the N-1->N delta the in-between blends. mario_vec==0 ⇒ the SDA
    // resolve failed and the shadow blend is a no-op.
    u32 mario_vec = 0; float mario_x = 0, mario_y = 0, mario_z = 0;
    float mario_dx = 0, mario_dy = 0, mario_dz = 0;
    // Silhouette-manager state probe: gpSilhouetteManager (r13-0x6094) and its unk48
    // (occlusion-darkening alpha) read BEFORE and AFTER the in-between redraw. If after!=before
    // the in-between mutated game state (the double unk48 advance + re-probe = the blink source).
    u32 sil_mgr = 0; float sil_before = 0, sil_after = 0;
    // marukage draw instrumentation (TSilhouette::perform 0x80227914), split by field. If
    // perform/&0x10 fire on the REAL field but not the in-between (or counts differ), the shadow
    // is a draw ASYMMETRY (on/off blink). If both fire equally, the blink is POSITION detach
    // (shadow center gpMarioPos at tick N vs the body's interpolated draw matrix).
    unsigned long sil_perf_real = 0, sil_perf_redraw = 0;   // perform() calls per field
    unsigned long sil_maru_real = 0, sil_maru_redraw = 0;   // &0x10 (marukage setup) per field
    u32 sil_lastparam_real = 0, sil_lastparam_redraw = 0;   // last param_1 seen each field
    // Screen-space flicker fix: on the in-between field, skip TEfbCtrlTex's EFB->screen-texture
    // copy so water/refraction (and the marukage on it) reuse the real field's capture. Default
    // ON; /interp60?skip_efbcopy=0 to A/B against the flicker.
    int skip_efbcopy = 0; unsigned long efbcopy_skipped = 0;   // reverted: reusing the stale screen
        // texture ghosts Mario into the reflection. Kept as a probe toggle only, default OFF.
};
extern Interp60Dbg g_i60;

// Probe entry point (called from /interp60). `query` is the raw path incl. args.
int interp60_probe(char* out, int cap, const char* query);
