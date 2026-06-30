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
bool gxp_parse_frame(const u8* p, size_t n, GxFrameInfo& out);
