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
#include <dolphin/gx/GXManage.h>   // GXDrawSyncCallback

namespace sb::platform::gx {

struct GXState {
    // --- transform: projection ---
    GXProjectionType projType;
    // Packed projection params, matching the decomp's gx->projMtx[6] / the layout
    // GXGetProjectionv emits and GXProject consumes (pm[1..6] = projMtx[0..5]).
    f32 projMtx[6];

    // --- transform: viewport (raw args as the game set them) ---
    f32 vpLeft, vpTop, vpWd, vpHt, vpNearz, vpFarz;

    // Latched PERSPECTIVE projection + the viewport that was live when it was set. The
    // native scene render (sms_boot_j3d_capture.cpp) reads these: GXSetProjection's live
    // projMtx is overwritten every frame by the 2D HUD's ortho before the model calc()
    // reads it, so the live value is the wrong (ortho) projection for the 3D scene. We
    // snapshot the last GX_PERSPECTIVE set so the scene render always has the 3D matrix.
    GXProjectionType proj3dType;
    f32 proj3dMtx[6];
    f32 proj3dVp[6];
    f32 proj3dM44[16];   // the full row-major 4x4 (C_MTXPerspective) — for clip-space project
    bool proj3dValid;

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

    // --- slice 5: vertex format / arrays / matrix memory / textures / texgen ---
    // These are the per-draw setters the J3D draw path invokes; the renderer reads
    // them when it consumes a shape. Captured as plain native fields (no FIFO).

    // GXSetVtxAttrFmt: per (vtxfmt 0..7, attr) component count/type/frac.
    struct VtxAttrFmt { u8 cnt; u8 type; u8 frac; } vtxAttrFmt[8][26];
    // GXSetArray: per attr the indexed-array base pointer + stride (POS/NRM/CLR/TEX).
    struct VtxArray { const void* base; u8 stride; } vtxArray[26];
    // GXSetVtxDesc/GXClearVtxDesc: immediate-mode vertex descriptor (NONE/DIRECT/INDEX*).
    u8 immVtxDesc[26];   // GXAttrType per attr (0=GX_NONE)

    // XF matrix memory. id in GXLoad*MtxImm is the matrix-memory ROW (the game uses
    // id = slot*3 for pos/nrm, slot*3 for tex). 64 slots of 3x4 covers GX_PNMTX0..63.
    f32  posMtx[64][3][4];   // GXLoadPosMtxImm / GXLoadPosMtxIndx (by row id/3)
    f32  nrmMtx[64][3][4];   // GXLoadNrmMtxImm / GXLoadNrmMtxIndx3x3 (3x3 in 3x4)
    f32  texMtx[64][3][4];   // GXLoadTexMtxImm
    u32  currentMtx;         // GXSetCurrentMtx (default pos/nrm matrix id)

    // GXSetTexCoordGen2: per dst texcoord, how it's generated.
    struct TexGen { u8 func; u8 src; u16 mtx; u8 normalize; u16 ptMtx; } texGen[8];

    // GXSetIndTexMtx: indirect-texture matrices (3 of them, GX_ITM_0..2).
    struct IndMtx { f32 offset[2][3]; s8 scaleExp; } indMtx[3];
    // GXSetIndTexCoordScale: per indirect stage, the S/T coord downscale.
    struct IndScale { u8 scaleS, scaleT; } indScale[4];

    // GXSetClipMode / GXSetCoPlanar / GXSetDither — pipeline flags.
    GXClipMode clipMode;
    GXBool     coPlanar;
    GXBool     dither;

    // GXInitTexObj writes a native descriptor into the caller-owned GXTexObj; on
    // GXLoadTexObj the bound descriptor for each texmap is recorded here for the
    // renderer. (NativeTexObj overlay defined in gx_impl.cpp.)
    struct BoundTex { bool valid; const void* image; u16 w, h; u8 fmt, wrapS, wrapT, mipmap;
                      u8 magFilt; u32 tlutName; } boundTex[8];

    // --- slice 6: framebuffer copy / pixel-format / fog / draw-sync / palettes ---
    // The display/management half of GX. "Rebuild as a PC game": these capture clean
    // semantic state (no GC BP-register packing, no FIFO). The pure-math helpers
    // (GXGetTexBufferSize, the XFB-line / yscale formulas) are ported verbatim from the
    // decomp (GXTexture.c / GXFrameBuf.c) and unit-tested; the rest record state the
    // native present path consults, or are documented no-ops where the GameCube hardware
    // concept has no native counterpart.

    // GXSetFog: clean semantic params (the decomp packs FP exponents into HW regs).
    struct Fog { GXFogType type; f32 startz, endz, nearz, farz; GXColor color; } fog;

    // GXSetDispCopySrc/Dst/Gamma/Frame2Field/YScale + GXSetCopyFilter/Clamp + GXCopyDisp.
    struct DispCopy {
        u16 srcLeft, srcTop, srcWd, srcHt;   // GXSetDispCopySrc
        u16 dstWd, dstHt;                     // GXSetDispCopyDst (xfb stride/height)
        GXGamma gamma;                        // GXSetDispCopyGamma
        GXCopyMode frame2field;               // GXSetDispCopyFrame2Field
        GXBool aa, vf;                        // GXSetCopyFilter enables
        GXFBClamp clamp;                      // GXSetCopyClamp
        f32 yScale;                           // last GXSetDispCopyYScale arg
        u32 yScaleLines;                      // its computed XFB-line return
        const void* lastDest;                 // last GXCopyDisp destination
    } dispCopy;

    // GXSetTexCopySrc/Dst + GXCopyTex: EFB->texture copy region.
    struct TexCopy {
        u16 srcLeft, srcTop, srcWd, srcHt, dstWd, dstHt;
        GXTexFmt fmt; GXBool mipmap;
        const void* lastDest;
    } texCopy;

    // GXSetPixelFmt / GXSetFieldMode / GXSetAlphaUpdate / GXSetDstAlpha / GXSetZTexture.
    GXPixelFmt pixFmt;  GXZFmt16 zFmt;
    GXBool fieldMode, halfAspect;
    GXBool alphaUpdate;
    GXBool dstAlphaEnable;  u8 dstAlpha;
    struct ZTex { GXZTexOp op; GXTexFmt fmt; u32 bias; } zTex;

    // GXSetIndTexOrder: per indirect stage, its texcoord + texmap (0xff = none).
    struct IndOrder { u8 texCoord, texMap; } indOrder[4];

    // GXSetDrawSync / GXSetDrawSyncCallback: the last token + the registered callback.
    u16 drawSyncToken;
    GXDrawSyncCallback drawSyncCb;

    // GXInitTlutObj/GXLoadTlut: bound palettes by tlut_name (GX_TLUT0..15 = 0..19).
    struct Tlut { const void* lut; u16 nEntries; u8 fmt; bool valid; } tlut[20];
};

// The single live GX state the seam writes and the renderer reads.
GXState& state();

} // namespace sb::platform::gx
