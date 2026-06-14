// port/pal/gx/gx_stub.cpp
//
// No-op stub layer for the GameCube GX graphics SDK.
//
// PURPOSE (throwaway, link-only): the decompiled SMS engine calls the GX
// immediate-mode graphics API (GXBegin/GXEnd, the TEV/texgen/blend state
// setters, matrix/light loads, display-list calls, ...). Real rendering will
// be wired into the native PC renderer later. For now these are empty no-ops
// so `libsmsport_core.a` (and anything that links it) RESOLVES every GX*
// symbol with a correctly-typed definition.
//
// We obtain the EXACT signatures by including the pristine GX SDK headers
// (extern "C" C linkage, matching the engine's expectations bit-for-bit) and
// then DEFINE each declared function as a no-op. Including the headers — rather
// than re-typing prototypes — guarantees the parameter types/linkage match, so
// the symbols actually resolve at link time (a silent signature mismatch would
// leave the symbol unresolved and the link still broken).
//
// Non-void returns yield a sane default (0 / nullptr). The few setters whose
// return value the engine could in principle branch on are flagged inline.
//
// Behavior is intentionally nil. Do NOT add real GPU logic here; that lives in
// the native renderer.

#include <dolphin/gx.h>  // pulls every dolphin/gx/*.h sub-header

// The GX headers wrap their declarations in `extern "C"`, so these definitions
// (compiled as C++) carry C linkage to match the undefined symbols.
extern "C" {

// ---------------------------------------------------------------------------
// Geometry / vertex description  (GXGeometry.h)
//   GXEnd / GXSetTexCoordGen are static-inline in the header (not symbols).
// ---------------------------------------------------------------------------
void GXSetVtxDesc(GXAttr, GXAttrType) {}
void GXSetVtxDescv(const GXVtxDescList*) {}
void GXClearVtxDesc(void) {}
void GXSetVtxAttrFmt(GXVtxFmt, GXAttr, GXCompCnt, GXCompType, u8) {}
void GXSetVtxAttrFmtv(GXVtxFmt, const GXVtxAttrFmtList*) {}
void GXSetArray(GXAttr, const void*, u8) {}
void GXInvalidateVtxCache(void) {}
void GXSetTexCoordGen2(GXTexCoordID, GXTexGenType, GXTexGenSrc, u32, GXBool,
                       u32) {}
void GXSetNumTexGens(u8) {}
void GXBegin(GXPrimitive, GXVtxFmt, u16) {}
void GXSetLineWidth(u8, GXTexOffset) {}
void GXSetPointSize(u8, GXTexOffset) {}
void GXEnableTexOffsets(GXTexCoordID, u8, u8) {}

// ---------------------------------------------------------------------------
// Cull  (GXCull.h)  — GXSetCullMode also declared in GXGeometry.h (same decl)
// ---------------------------------------------------------------------------
void GXSetScissor(u32, u32, u32, u32) {}
void GXSetCullMode(GXCullMode) {}
void GXSetCoPlanar(GXBool) {}

// ---------------------------------------------------------------------------
// Transform / matrices / viewport  (GXTransform.h)
// ---------------------------------------------------------------------------
void GXSetProjection(f32[4][4], GXProjectionType) {}
void GXLoadPosMtxImm(f32[3][4], u32) {}
void GXLoadPosMtxIndx(u16, u32) {}
void GXLoadNrmMtxImm(f32[3][4], u32) {}
void GXLoadNrmMtxIndx3x3(u16, u32) {}
void GXSetCurrentMtx(u32) {}
void GXLoadTexMtxImm(f32[][4], u32, GXTexMtxType) {}
void GXSetViewport(f32, f32, f32, f32, f32, f32) {}
void GXSetClipMode(GXClipMode) {}

// ---------------------------------------------------------------------------
// TEV (texture environment)  (GXTev.h)
// ---------------------------------------------------------------------------
void GXSetTevOp(GXTevStageID, GXTevMode) {}
void GXSetTevColorIn(GXTevStageID, GXTevColorArg, GXTevColorArg, GXTevColorArg,
                     GXTevColorArg) {}
void GXSetTevAlphaIn(GXTevStageID, GXTevAlphaArg, GXTevAlphaArg, GXTevAlphaArg,
                     GXTevAlphaArg) {}
void GXSetTevColorOp(GXTevStageID, GXTevOp, GXTevBias, GXTevScale, GXBool,
                     GXTevRegID) {}
void GXSetTevAlphaOp(GXTevStageID, GXTevOp, GXTevBias, GXTevScale, GXBool,
                     GXTevRegID) {}
void GXSetTevColor(GXTevRegID, GXColor) {}
void GXSetTevColorS10(GXTevRegID, GXColorS10) {}
void GXSetTevKColor(GXTevKColorID, GXColor) {}
void GXSetTevKColorSel(GXTevStageID, GXTevKColorSel) {}
void GXSetTevKAlphaSel(GXTevStageID, GXTevKAlphaSel) {}
void GXSetTevSwapMode(GXTevStageID, GXTevSwapSel, GXTevSwapSel) {}
void GXSetTevSwapModeTable(GXTevSwapSel, GXTevColorChan, GXTevColorChan,
                           GXTevColorChan, GXTevColorChan) {}
void GXSetAlphaCompare(GXCompare, u8, GXAlphaOp, GXCompare, u8) {}
void GXSetZTexture(GXZTexOp, GXTexFmt, u32) {}
void GXSetTevOrder(GXTevStageID, GXTexCoordID, GXTexMapID, GXChannelID) {}
void GXSetNumTevStages(u8) {}

// ---------------------------------------------------------------------------
// Indirect texturing / bump  (GXBump.h)
// ---------------------------------------------------------------------------
void GXSetTevIndirect(GXTevStageID, GXIndTexStageID, GXIndTexFormat,
                      GXIndTexBiasSel, GXIndTexMtxID, GXIndTexWrap,
                      GXIndTexWrap, GXBool, GXBool, GXIndTexAlphaSel) {}
void GXSetIndTexMtx(GXIndTexMtxID, f32[2][3], s8) {}
void GXSetIndTexCoordScale(GXIndTexStageID, GXIndTexScale, GXIndTexScale) {}
void GXSetIndTexOrder(GXIndTexStageID, GXTexCoordID, GXTexMapID) {}
void GXSetNumIndStages(u8) {}
void GXSetTevDirect(GXTevStageID) {}

// ---------------------------------------------------------------------------
// Texture objects / TLUTs / cache regions  (GXTexture.h)
// ---------------------------------------------------------------------------
void GXInitTexObj(GXTexObj*, void*, u16, u16, GXTexFmt, GXTexWrapMode,
                  GXTexWrapMode, u8) {}
void GXInitTexObjLOD(GXTexObj*, GXTexFilter, GXTexFilter, f32, f32, f32, GXBool,
                     GXBool, GXAnisotropy) {}
void GXLoadTexObj(GXTexObj*, GXTexMapID) {}
void GXInitTlutObj(GXTlutObj*, void*, GXTlutFmt, u16) {}
void GXLoadTlut(GXTlutObj*, u32) {}
void GXInitTexCacheRegion(GXTexRegion*, u8, u32, GXTexCacheSize, u32,
                          GXTexCacheSize) {}
void GXSetTexCoordScaleManually(GXTexCoordID, u8, u16, u16) {}
void GXSetTexCoordCylWrap(GXTexCoordID, u8, u8) {}

// ---------------------------------------------------------------------------
// Pixel / fog / blend / Z  (GXPixel.h)
// ---------------------------------------------------------------------------
void GXSetFog(GXFogType, f32, f32, f32, f32, GXColor) {}
void GXSetFogRangeAdj(GXBool, u16, GXFogAdjTable*) {}
void GXSetBlendMode(GXBlendMode, GXBlendFactor, GXBlendFactor, GXLogicOp) {}
void GXSetColorUpdate(GXBool) {}
void GXSetAlphaUpdate(GXBool) {}
void GXSetZMode(GXBool, GXCompare, GXBool) {}
void GXSetZCompLoc(GXBool) {}
void GXSetDither(GXBool) {}
void GXSetDstAlpha(GXBool, u8) {}

// ---------------------------------------------------------------------------
// Lighting / channels  (GXLighting.h)
// ---------------------------------------------------------------------------
void GXInitLightAttnA(GXLightObj*, f32, f32, f32) {}
void GXInitLightAttnK(GXLightObj*, f32, f32, f32) {}
void GXInitLightPos(GXLightObj*, f32, f32, f32) {}
void GXInitLightColor(GXLightObj*, GXColor) {}
void GXLoadLightObjImm(GXLightObj*, GXLightID) {}
void GXSetChanAmbColor(GXChannelID, GXColor) {}
void GXSetChanMatColor(GXChannelID, GXColor) {}
void GXSetNumChans(u8) {}
void GXSetChanCtrl(GXChannelID, GXBool, GXColorSrc, GXColorSrc, u32,
                   GXDiffuseFn, GXAttnFn) {}

// ---------------------------------------------------------------------------
// Display lists  (GXDispList.h)
// ---------------------------------------------------------------------------
void GXCallDisplayList(void*, u32) {}

// ---------------------------------------------------------------------------
// Manage / misc  (GXManage.h)
// ---------------------------------------------------------------------------
void GXSetMisc(GXMiscToken, u32) {}

}  // extern "C"
