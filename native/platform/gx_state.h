// gx_state.h — the native GX state context.
//
// "Rebuild as a PC game", NOT a GX FIFO emulator: the game's GXSet*/GXLoad* calls
// capture render STATE into this plain native struct, and the native renderer (ngx)
// reads it — there is NO GameCube command FIFO, no XF/BP register stream. This is the
// decomp's global `gx` struct, reborn as host state the renderer consults directly.
//
// Grows seam-slice by seam-slice. Slice 1 (landed): the transform block (projection +
// viewport + scissor) consumed by GXProject and the renderer's eye->screen transform.
#pragma once
#include <dolphin/types.h>
#include <dolphin/gx/GXEnum.h>

namespace sb::platform::gx {

struct GXState {
    // --- transform: projection ---
    GXProjectionType projType;
    // Packed projection params, matching the decomp's gx->projMtx[6] / the layout
    // GXGetProjectionv emits and GXProject consumes (pm[1..6] = projMtx[0..5]).
    f32 projMtx[6];

    // --- transform: viewport (raw args as the game set them) ---
    f32 vpLeft, vpTop, vpWd, vpHt, vpNearz, vpFarz;

    // --- transform: scissor rect ---
    u32 scLeft, scTop, scWd, scHt;

    // --- pixel pipeline (clean semantic fields, NOT GC BP register packing) ---
    // blend
    GXBlendMode   blendType;
    GXBlendFactor blendSrc, blendDst;
    GXLogicOp     blendLogicOp;
    // z-buffer
    GXBool   zCompare, zUpdate;
    GXCompare zFunc;
    GXBool   zCompLocBeforeTex;   // GXSetZCompLoc
    // cull / framebuffer update
    GXCullMode cullMode;
    GXBool     colorUpdate;
    // alpha compare
    GXCompare alphaComp0, alphaComp1;
    u8        alphaRef0, alphaRef1;
    GXAlphaOp alphaOp;
    // copy clear
    GXColor copyClearColor;
    u32     copyClearZ;
    // pipeline counts
    u8 numChans, numTexGens, numTevStages;
};

// The single live GX state the seam writes and the renderer reads.
GXState& state();

} // namespace sb::platform::gx
