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
};

// Parse `n` bytes of frame stream; fills `out`. Returns out.ok.
bool gxp_parse_frame(const u8* p, size_t n, GxFrameInfo& out);
