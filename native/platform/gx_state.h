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

    // --- lighting (GXSetChanCtrl/GXSetChanMatColor/GXSetChanAmbColor) ---
    // Per colour channel, indexed by GXChannelID (GX_COLOR0=0, GX_COLOR1=1,
    // GX_ALPHA0=2, GX_ALPHA1=3; GX_COLOR0A0/COLOR1A1 set the colour+alpha pair).
    // `ctrl` is packed in the layout ngx_light's decode_chanctl() consumes:
    //   b0 matSrc(REG=0/VTX=1)  b1 enable  b2..b5 lightMask[0..3]  b6 ambSrc
    //   b7..b8 diffuseFn  b9..b10 attnFn(0/2=NONE,1=SPEC,3=SPOT)  b11..b14 lightMask[4..7]
    struct ChanState {
        u32     ctrl;
        GXColor matColor;
        GXColor ambColor;
    } chan[4];

    // --- light objects (GXInitLight*/GXLoadLightObjImm) in a native layout ---
    // The opaque GXLightObj is owned natively (see gx_impl.cpp NativeLightObj), so the
    // loaded state is a friendly float form the renderer turns into ngx::LightSrc.
    struct LightState {
        bool valid;
        f32  color[4];     // RGBA 0..1
        f32  pos[3];       // eye/world-space position
        f32  dir[3];       // (half-angle) direction
        f32  cosAtt[3];    // angle attenuation a0,a1,a2
        f32  distAtt[3];   // distance attenuation k0,k1,k2
    } light[8];
};

// The single live GX state the seam writes and the renderer reads.
GXState& state();

} // namespace sb::platform::gx
