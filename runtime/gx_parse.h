#pragma once
// GX frame-stream analyzer — milestone P of the 60 fps interpolation arc.
//
// Parses one captured frame's gather-pipe byte stream (gx_stream.cpp) using
// Dolphin's own OpcodeDecoder + VertexLoader size math (no duplicated GX
// knowledge). Produces exactly what the interpolation replay needs:
//   - offsets of PE draw-sync token BP loads (0x47/0x48) — a replayed frame
//     must not re-emit tokens or the native TDrawSyncManager double-retires
//   - offsets+values of CP ARRAY_BASE loads for XF_A (pos-matrix array, J3D
//     setModelDrawMtx) and XF_B (normal-matrix array) — the patch points that
//     redirect a replay to blended shadow matrix buffers
//   - integrity counters: a frame only qualifies for replay if it parsed to
//     the exact end with no unknown opcodes.
//
// CP state (VCD/VAT/arrays) persists across frames in the analyzer — J3D
// reprograms it per shape, so state converges within the first armed frame.

#include "cpu_state.h"
#include <vector>

struct GxFrameInfo {
    std::vector<u32> token_offsets;          // byte offsets of 5-byte BP token cmds
    struct ArrayPatch { u32 offset; u32 array; u32 base; };  // CP ARRAY_BASE loads
    std::vector<ArrayPatch> mtx_arrays;      // array 12 = XF_A (pos), 13 = XF_B (nrm)
    u32 prims = 0, display_lists = 0, copies = 0;

    // ── EFB-copy SEQUENCE (render-target structure oracle) ────────────────────────────────────────
    // Each BPMEM_TRIGGER_EFB_COPY (GXCopyDisp/GXCopyTex) is a render-target boundary: the EFB is
    // snapshotted out (to a TEXTURE for a later sampler, or to the XFB for display) and usually
    // cleared for the next pass. The native renderer flattens every pass into ONE framebuffer with
    // ONE clear, so an intra-frame EFB→texture copy that Dolphin makes (e.g. the file-select unk40
    // pre-pass output) has no native equivalent — native keeps compositing, double-drawing the
    // scene. Record each copy in order with: the BP value (bit14=copy-to-XFB, bit11=clear), and the
    // cumulative prim count BEFORE it, so a tool can diff the pass structure directly (not by pixels).
    struct EfbCopy { u32 offset; u32 value; u32 prims_before; bool to_xfb; bool clear; };
    std::vector<EfbCopy> efb_copies;

    // ── Per-draw BLEND / TEV value oracle (file-select overbright, 2026-06-30) ─────────────────────
    // The dominant file-select overbright lives in the POST pass (mPerformListGXPost) — a multi-layer
    // blend the native renderer over-composites (the CLAUDE.md "no-oracle trap"). To fix it by VALUE
    // (not pixel ablation) we need Dolphin's ground-truth blend equation + TEV stage count for the
    // draws in each EFB pass, so a tool can diff them against native's per-batch blend (sms_boot_present
    // batchdbg `bm=s/d/...`). Each draw records the live GX pixel state at draw time: blend factors,
    // subtract/enable/logicop, color/alpha update, and the TEV stage count (BPMEM_GENMODE numtevstages).
    // `efb_pass` = how many EFB copies have fired before this draw (0 = pre-first-copy pass, etc.), so
    // the POST pass = the highest efb_pass. `prims` = num_vertices of the draw (0 for a bare DL header).
    struct DrawRec {
        u32 offset;          // stream offset of the primitive/DL command
        u8  efb_pass;        // EFB-copy index this draw falls in (0,1,2,…)
        u8  src, dst;        // GX blend src/dst factor (BlendMode.src_factor / dst_factor)
        u8  blend_enable;    // BlendMode.blend_enable
        u8  subtract;        // BlendMode.subtract (1 => GX_BM_SUBTRACT)
        u8  logic_enable;    // BlendMode.logic_op_enable
        u8  logic_mode;      // BlendMode.logic_mode
        u8  color_update;    // BlendMode.color_update
        u8  alpha_update;    // BlendMode.alpha_update
        u8  numtevstages;    // BPMEM_GENMODE.numtevstages + 1 (live)
        u8  proj_type;       // 0 perspective / 1 ortho at draw time
        u32 prims;           // num_vertices of this primitive (0 if a DL header record)
    };
    std::vector<DrawRec> draws;

    // ── Per-draw per-STAGE TEV combiner snapshot (file-select sea-water overbright, 2026-06-30) ─────
    // The blend oracle (DrawRec) gives the blend equation + stage count; this gives the combiner
    // CONTENTS so the native sea-water TEV (b76, which paints opaque-white) can be diffed register-for-
    // register against Dolphin's. Captured only when SUNBRIGHT_DBG_GXTEV is set (it copies up to 16
    // stage register pairs per draw), parallel to `draws` (same index). color_env/alpha_env are the live
    // BPMEM_TEV_COLOR_ENV/ALPHA_ENV words — identical layout to NgxTevStage::color_env/alpha_env.
    struct TevSnap {
        u8  nstages;             // active TEV stages at draw time (GENMODE.numtevstages + 1)
        u32 color_env[16];       // per-stage ColorCombiner reg (BPMEM_TEV_COLOR_ENV, 0xC0 + 2*stage)
        u32 alpha_env[16];       // per-stage AlphaCombiner reg (BPMEM_TEV_ALPHA_ENV, 0xC1 + 2*stage)
        u32 tevreg_ra[4];        // TEV color/konst regs RA word (BPMEM_TEV_COLOR_RA, 0xE0 + 2*reg)
        u32 tevreg_bg[4];        // TEV color/konst regs BG word (BPMEM_TEV_COLOR_BG, 0xE1 + 2*reg)
    };
    std::vector<TevSnap> tev_snaps;   // empty unless SUNBRIGHT_DBG_GXTEV; tev_snaps[i] pairs with draws[i]

    // ── Per-pass geometry split (cross-engine pass tagging) ───────────────────────────────────────
    // The native parity dump (sb_parity_dump.h) reports the 3D SCENE pass only (perspective), so the
    // whole-frame oracle counts are NOT directly comparable. Bucket prims/verts/display-lists by the
    // ACTIVE projection type at draw time — the reliable, renderer-neutral pass discriminator both
    // engines share: index 0 = perspective (the 3D scene = "scene" pass), 1 = ortho (the 2D HUD/overlay
    // = "hud" pass). `verts_pass` is the real summed vertex count (OnPrimitiveCommand num_vertices),
    // so the SCENE bucket is directly comparable to the native dump's nverts. See gx_capture.cpp.
    u32 prims_pass[2] = {0, 0};
    u32 verts_pass[2] = {0, 0};
    u32 dls_pass[2]   = {0, 0};
    // Per-pass AMBIENT: the frame-global `amb` (below) is "last seen" = the frame-FINAL value (the J2D
    // HUD sets it to white 255 AFTER the scene), so comparing it to native's scene-draw ambient is
    // apples-to-oranges. Record the live SETCHAN0_AMBCOLOR at the moment of each primitive draw into the
    // active pass so the SCENE bucket carries the ambient the GPU actually used for the 3D scene.
    float amb_pass[2][3] = {{0,0,0},{0,0,0}};
    bool  amb_pass_set[2] = {false, false};

    bool ok = false;                         // parsed exactly to the end, no unknowns
    // failure forensics
    u32 fail_offset = 0;                     // where parsing stopped
    u8  fail_opcode = 0;                     // opcode byte there
    u32 total = 0;                           // frame size

    // ── Per-pass parity oracle (Dolphin-GX ground truth from the command stream) ──────────────────
    // The XF register state the GPU actually uses, captured from the SETPROJECTION / SETVIEWPORT /
    // light-memory / channel-colour XF loads in this frame's stream. This is the VALID oracle for the
    // native renderer's lighting/projection parity (sb_parity_dump.h) — it is what Dolphin's GX
    // pipeline consumes, not the async-lagged xfmem read-back. Reset per frame.
    struct GxLight { bool valid = false; float pos[3] = {}; float color[3] = {}; };
    GxLight lights[8];
    int   light_loads = 0;                   // count of light-memory XF loads seen (8 lights max)
    float amb[3]  = {0,0,0};                  // SETCHAN0_AMBCOLOR (u8 RGB -> 0..1), last seen
    float matc[4] = {1,1,1,1};               // SETCHAN0_MATCOLOR (u8 RGBA -> 0..1), last seen
    bool  have_proj = false;
    int   proj_type = 0;                     // 0 = perspective, 1 = orthographic
    float proj[6] = {};                      // SETPROJECTION matrix (6 values)
    bool  have_vp = false;
    float vp[6] = {};                        // SETVIEWPORT (wd,ht,nearz, xorig,yorig,farz scaled)
    u32   chan0_ctrl = 0;                     // SETCHAN0_COLOR (light mask / amb-mat source)
};

// Parse `n` bytes of frame stream; fills `out`. Returns out.ok.
// `recurse_dls` (parity oracle only): follow GX_CMD_CALL_DL into guest RAM so display-list prims/verts
// are counted into verts_pass/prims_pass — needed because the DL bodies are NOT in the FIFO stream.
// Default off so the interpolation consumers (token/matrix-array offsets) see unchanged behaviour.
bool gxp_parse_frame(const u8* p, size_t n, GxFrameInfo& out, bool recurse_dls = false);
