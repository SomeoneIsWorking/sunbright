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

    // --- TEV combiner state (GXSetTev*/GXSetTevColor/GXSetTevKColor*) ---
    // Captured in the SAME bit layout the GameCube BP TEV registers use — which is
    // exactly NgxTevState.color_env/alpha_env (the J3D path stores those registers raw),
    // so the renderer's tev_shader decodes this directly (ngx_tevstate_from_gx bridges
    // GXState -> NgxTevState). colorEnv/alphaEnv per stage are gx->tevc/teva; kcsel/kasel
    // and swap selection are kept UNPACKED (NgxTevStage form) for the shader.
    struct TevBlock {
        u32 colorEnv[16];   // per stage: a@12 b@8 c@4 d@0, op@18, scale@20/bias@16, clamp@19, dest@22
        u32 alphaEnv[16];   // per stage: a@13 b@10 c@7 d@4, op@18, ..., dest@22, rasSwap@0, texSwap@2
        u8  kcsel[16];      // GXTevKColorSel per stage
        u8  kasel[16];      // GXTevKAlphaSel per stage
        u8  texcoord[16];   // GX_TEXCOORD0.. (0xff = none)
        u8  texmap[16];     // GX_TEXMAP0..   (0xff = none)
        u8  colorChan[16];  // GXChannelID raster source
        // The 4 swap tables (NgxTevState swizzle byte: r=(b>>6)&3 g=(b>>4)&3 b=(b>>2)&3 a=b&3).
        // Default to identity 0x1B (what GXInit programs table 0); a 0 would swizzle "rrrr".
        u8  swapTable[4] = { 0x1B, 0x1B, 0x1B, 0x1B };
        s16 tevColor[4][4]; // GXSetTevColor[S10] CPREV/C0/C1/C2 RGBA (S10)
        u8  kColor[4][4];   // GXSetTevKColor KONST0..3 RGBA (0..255)
        u8  numIndStages;   // GXSetNumIndStages
    } tev;
};

// The single live GX state the seam writes and the renderer reads.
GXState& state();

} // namespace sb::platform::gx
