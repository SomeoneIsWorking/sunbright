// gx_seam.h — native reimplementation of the GC GX graphics API.
//
// SDK headers replaced: reference/sms/include/dolphin/gx/*.h, gx.h
// Used surface: 192 distinct GX functions, 3070 calls — the HOTTEST seam.
//
// CORE PRINCIPLE (standing project rule, see CLAUDE.md "Native renderer GROUND
// RULES"): this is a NATIVE Super Mario Sunshine renderer, NOT GameCube GX FIFO
// emulation. The game builds J3D models / J2D screens and submits immediate-mode
// geometry + sets pipeline state through GX*; the seam records that state into a
// native GX context and routes draws to the ngx renderer (runtime/render/ +
// runtime/ngx/). We do NOT decode a GP FIFO or tap emulated registers.
//
// The GX API splits into three kinds of entry point:
//   1. STATE SETTERS (GXSetTevOrder, GXSetBlendMode, GXSetChanCtrl, GXSetVtxDesc,
//      GXLoadPosMtxImm, GXInitTexObj/GXLoadTexObj, ...) -> mutate the native GX
//      context (a plain native struct mirroring TEV/PE/XF/texgen state).
//   2. DRAW VERBS (GXBegin/GXPosition*/GXTexCoord*/GXColor*/GXEnd, GXDrawSphere,
//      GXCallDisplayList) -> assemble a native vertex batch tagged with the current
//      GX-context snapshot, hand it to the ngx batch pipeline.
//   3. FRAMEBUFFER/MANAGE (GXSetCopyClear, GXCopyDisp, GXCopyTex, GXSetViewport,
//      GXDrawDone, GXFlush) -> present / EFB-copy-to-texture / drawsync, handled by
//      ngx_present.cpp + ngx_efb_copy.h.
//
// REUSE: ngx already implements vertex decode (ngx_vertex), TEV combiner
// (tev_eval.h), lighting (ngx_light.h), texgen, textures (tex_decode.cpp),
// indirect texturing (ngx_indirect.h), PE block, present (ngx_present.cpp), J2D/HUD
// (j2d_walk.cpp). The seam is a thin native re-expression of GX entry points onto
// that pipeline operating on NATIVE structs (vs guest-RAM layout under the recomp).
//
// This header declares the GX context + the *categories* of entry point. Phase-2
// fills each GXSet*/GX* with a body that pokes the context or emits a batch. The
// full 192-function list lives in api_surface.md; the SDK signatures are in the
// gx/*.h headers (read-only) — match them exactly so the game links.
#pragma once
#include "platform_types.h"

namespace sb::platform::gx {

// Forward decl of the native GX render context (TEV stages, PE/blend/zmode,
// channel/lighting, texgen, current matrices, vtx descriptors, bound tex objs).
// Phase-2: define this mirroring the GX register/XF state ngx already consumes.
struct GxContext;
GxContext& Ctx();   // the singleton native GX state

// ---- management (gx/GXManage.h, GXFifo.h) -------------------------------
void  Init();            // GXInit -> create the native GX context + ngx renderer
void  Flush();           // GXFlush -> flush the current batch
void  DrawDone();        // GXDrawDone -> drawsync barrier (ngx)
void  SetDrawDone();     // GXSetDrawDone
void  AbortFrame();      // GXAbortFrame

// ---- (1) STATE SETTERS — categories; each GXSet* mutates Ctx() -----------
// geometry/vtx descriptors (gx/GXGeometry.h, GXVert.h): GXSetVtxDesc,
//   GXSetVtxAttrFmt, GXClearVtxDesc, GXSetCurrentMtx, GXSetNumChans/TexGens/...
// transform (gx/GXTransform.h): GXLoadPosMtxImm, GXLoadNrmMtxImm, GXSetProjection,
//   GXSetViewport, GXSetScissor.
// TEV (gx/GXTev.h): GXSetTevOrder, GXSetTevColorIn/AlphaIn, GXSetTevColorOp/AlphaOp,
//   GXSetTevColor, GXSetNumTevStages, GXSetTevSwapMode*.
// lighting (gx/GXLighting.h): GXSetChanCtrl, GXSetChanMatColor, GXSetChanAmbColor,
//   GXInitLightAttn/Pos/Dir, GXLoadLightObjImm.
// pixel/blend (gx/GXPixel.h): GXSetBlendMode, GXSetZMode, GXSetAlphaUpdate,
//   GXSetColorUpdate, GXSetAlphaCompare, GXSetZCompLoc, GXSetDither.
// cull (gx/GXCull.h): GXSetCullMode, GXSetCoPlanar.
// (NOTE: GX_WRITE_BP_REG, 183 call sites, is a macro poking the BP register file —
//  in the native build these resolve to native Ctx() mutations, not FIFO writes.)
// TODO phase-2: one function per SDK signature, each writing Ctx().

// ---- texture (gx/GXTexture.h) -------------------------------------------
// GXInitTexObj/GXInitTexObjCI, GXLoadTexObj, GXInitTlutObj, GXLoadTlut,
// GXInitTexObjLOD. Decode via tex_decode.cpp; bind into Ctx() texmap slots.
// TODO phase-2.

// ---- (2) DRAW VERBS (gx/GXGeometry.h, GXVert.h, GXDraw.h) ----------------
// GXBegin(primType, vtxFmt, count) opens a batch; GXPosition3f32/3s16,
// GXNormal3f32, GXTexCoord2f32, GXColor1u32/4u8 push attributes per the active
// vtx descriptor; GXEnd closes it -> emit a native batch (current Ctx() snapshot
// + decoded vertices) to ngx. GXDrawSphere/Cube/Cylinder are primitive helpers.
// GXCallDisplayList replays a built display list -> walk it natively into batches.
// TODO phase-2: route to the ngx batch assembler (ngx_mesh / ngx_imm_geom).

// ---- (3) FRAMEBUFFER / PRESENT (gx/GXFrameBuffer.h, GXCpu2Efb.h) ---------
struct Color { u8 r, g, b, a; };   // mirrors GXColor (passed BY POINTER in SMS GX!)
void SetCopyClear(const Color* clear, u32 clearZ);  // GXSetCopyClear (ptr arg!)
void CopyDisp(void* dest, bool clear);              // GXCopyDisp -> present (ngx)
void CopyTex(void* dest, bool clear);               // GXCopyTex -> EFB-copy-to-tex
void SetViewport(f32 x, f32 y, f32 w, f32 h, f32 nearZ, f32 farZ); // GXSetViewport
// EFB peek/poke (gx/GXFrameBuffer.h): GXPeekARGB / GXPeekZ — read back native
// scene color/depth (occlusion, sun, pollution). ngx has depth/color readback.
// TODO phase-2.

} // namespace sb::platform::gx
