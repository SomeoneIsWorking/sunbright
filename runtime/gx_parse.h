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
        u8  immediate;       // 1 = top-level in-FIFO GXBegin (dl_depth==0); 0 = inside a display list
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

    // ── Per-draw LIGHTING snapshot (overbright wash investigation, 2026-07-02) ──────────────────
    // The blend + TEV snapshots (above) explain WHAT combiner runs; this explains what INPUTS the
    // combiner is fed via the chan-ctrl / lighting pipeline. Snapshots the live lighting state at
    // the moment of each primitive so an oracle vs native per-shape diff can pinpoint which stage
    // diverges: the per-vertex material colour (matc), the ambient (amb), the chan-ctrl (which
    // lights are enabled + which vertex source is used), and the 8 lights themselves. Gated on
    // SUNBRIGHT_DBG_GXLIGHT (or implied by SUNBRIGHT_DBG_GXTEV so a single flag covers the whole
    // "why is native's ph6 lighting different from oracle's" diagnostic). Layout mirrors what
    // native's batchtev/batchlight already emit — same fields on both sides.
    struct LightSnap {
        u32   chan0_ctrl;      // BPMEM_CHAN0_COLOR (0x100e): light mask + amb-mat source flags
        float amb[3];          // live BPMEM_CHAN0_AMBCOLOR
        float matc[4];         // live BPMEM_CHAN0_MATCOLOR
        u8    light_valid;     // bitmask: bit i = light i is enabled+loaded
        float light_pos[8][3]; // per-light position at draw time
        float light_col[8][3]; // per-light colour at draw time
    };
    std::vector<LightSnap> light_snaps;   // empty unless SUNBRIGHT_DBG_GXLIGHT; light_snaps[i] pairs with draws[i]

    // ── On-wire GXSetColorUpdate transitions (2026-07-02, cU-dispatch probe) ─────────────────────
    // Every BPMEM_BLENDMODE write whose color_update bit differs from the previous BlendMode is a GC
    // on-wire GXSetColorUpdate() effect. Recording the stream offset lets a downstream consumer
    // (gx_capture) correlate each transition to the nearest preceding gather-flush guest-PC (FlushMark)
    // — naming the specific game function that wrote cU=FALSE around a depth-only prepass. Native's
    // SB_COLUPD_ALL emits the same information from the sms-boot side; diffing the two lists names the
    // dispatch path missing on native. Always captured (cost = one push_back per real cU transition).
    struct CuWrite { u32 offset; u8 new_cU; u8 old_cU; };
    std::vector<CuWrite> cu_writes;

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
    // Per-pass PROJECTION+VIEWPORT: the frame-global proj/vp below is "last seen" (= HUD ortho by
    // frame end), so the scene pass line would carry HUD state and the state-pin fingerprint would
    // reject a true match. Latch at the FIRST primitive of each pass, same discipline as amb_pass.
    int   proj_type_pass[2] = {0, 0};
    float proj_pass[2][6]   = {{0,0,0,0,0,0},{0,0,0,0,0,0}};
    bool  proj_pass_set[2]  = {false, false};
    float vp_pass[2][6]     = {{0,0,0,0,0,0},{0,0,0,0,0,0}};
    bool  vp_pass_set[2]    = {false, false};
    // Per-pass POSITION MATRIX (row-major 3x4, XFmem 0x000-based PNMTX0 = current position matrix
    // at first primitive of the pass). Latched once per pass, same discipline as proj_pass/vp_pass.
    // Used for the sky-dome / camera projection audit (sky #16, 2026-07-04): native captures the
    // equivalent 3x4 via j3dSys draw-matrix table + sb_gx_get_live_projection; a diff of the two
    // 3x4s + proj[6] + vp[6] names the divergence in the view/camera chain.
    float posmtx_pass[2][12] = {{0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0}};
    bool  posmtx_pass_set[2] = {false, false};
    // Per-EFB-pass projection/viewport/PNMTX0 latch (2026-07-04). proj_pass above latches at the
    // very first perspective primitive, which on SMS title is the mirror-camera pre-pass @ FOVy=52°
    // (TMirrorCamera::perform, memory [[session16-perpass-fingerprint]]) — NOT the main scene draw
    // where sky.bmd's dome renders at FOVy=40°. Track per-EFB-copy-pass so a tool can pick out the
    // main scene (efb_pass ≥ 1, i.e. AFTER the mirror pre-copy). Up to 8 passes tracked.
    int   proj_type_efb[8] = {0,0,0,0,0,0,0,0};
    float proj_efb[8][6]   = {};
    bool  proj_efb_set[8]  = {};
    float vp_efb[8][6]     = {};
    bool  vp_efb_set[8]    = {};
    float posmtx_efb[8][12] = {};
    bool  posmtx_efb_set[8] = {};
    // Immediate-mode (in-FIFO GXBegin) vertex count per pass, split from display-list verts. The
    // map/scene geometry issues via GX_CMD_CALL_DL (display lists); TMapObjWave's sea grid draws via
    // raw immediate-mode GXBegin. So imm_verts_pass[scene] isolates the wave-class draw in the oracle —
    // the definitive test of whether the oracle draws a separate immediate sea overlay at all.
    u32 imm_verts_pass[2] = {0, 0};
    // Triangle count per pass — the triangulation-invariant geometry metric (strips/fans/quads all
    // reduced to triangles). Comparable to native's g_verts.size()/3, unlike the raw-vs-list vert
    // counts. This is the RELIABLE cross-engine geometry-parity number.
    u32 tris_pass[2] = {0, 0};
    // Triangles per EFB-copy pass (efb_pass index). GC renders across multiple EFB targets — the main
    // scene renders off-screen, then a display/GXPost pass RE-DRAWS/composites several buffers. Those
    // re-draws inflate the whole-frame triangle count, so native (a single-target flat compositor that
    // does NOT re-draw) reads as an "under-draw" vs the oracle TOTAL. Splitting by efb_pass isolates
    // the MAIN scene pass (efb 0) — the geometry native should actually match — from the composite.
    u32 tris_efb[8] = {0,0,0,0,0,0,0,0};
    u32 max_efb_pass = 0;

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
