// gx_fb_impl.cpp — native GX seam, SLICE 6: the framebuffer/management half of GX
// (display & texture copy, pixel format, fog, draw-sync, palettes, the imm-mode draw
// verbs, peek, perf metrics) + the pure-math texture/XFB helpers and the NTSC-480i
// render-mode data symbol. Continues the gx_impl.cpp pattern: "rebuild as a PC game",
// NOT a GameCube FIFO emulator — GXSet* capture clean semantic state into GXState
// (gx_state.h); the GC BP/CP/XF register & FIFO writes are DROPPED.
//
// Two classes of function live here:
//   * PURE MATH ported VERBATIM from the decomp (GXTexture.c / GXFrameBuf.c) — texture
//     buffer sizing, XFB-line / Y-scale formulas. These are deterministic and are
//     UNIT-TESTED (gx_test.cpp slice-6 cases) against hand-computed ground truth.
//   * STATE / no-ops — copy/pixel/fog/sync setters record into GXState for the native
//     present path; entries with no native hardware counterpart (perf metrics, FIFO
//     pointers, breakpoints, point size, EFB peek without an EFB in this lib) are
//     DOCUMENTED no-ops. The immediate-mode draw verbs (GXDrawCube/Sphere) are inert in
//     this base lib exactly like GXBegin — the ngx layer captures geometry separately.

#include <dolphin/gx.h>
#include <dolphin/os.h>   // OSPanic (fail-fast on a divide-by-zero copy scale)
#include "gx_state.h"

using sb::platform::gx::state;

// ===========================================================================
// Pure-math helpers — ported verbatim from the decomp (no FIFO, deterministic).
// ===========================================================================
namespace {

// GXTexture.c __GXGetTexTileShift: row/col tile shift (log2 of tile dims) per format.
void GetTexTileShift(u32 fmt, u32* rowTileS, u32* colTileS) {
    switch (fmt) {
    case GX_TF_I4: case 0x8 /*GX_TF_C4*/: case GX_TF_CMPR:
    case GX_CTF_R4: case GX_CTF_Z4:
        *rowTileS = 3; *colTileS = 3; break;
    case GX_TF_I8: case GX_TF_IA4: case 0x9 /*GX_TF_C8*/: case GX_TF_Z8:
    case GX_CTF_RA4: case GX_CTF_A8: case GX_CTF_R8: case GX_CTF_G8:
    case GX_CTF_B8: case GX_CTF_Z8M: case GX_CTF_Z8L:
        *rowTileS = 3; *colTileS = 2; break;
    case GX_TF_IA8: case GX_TF_RGB565: case GX_TF_RGB5A3: case GX_TF_RGBA8:
    case 0xA /*GX_TF_C14X2*/: case GX_TF_Z16: case GX_TF_Z24X8:
    case GX_CTF_RA8: case GX_CTF_RG8: case GX_CTF_GB8: case GX_CTF_Z16L:
        *rowTileS = 2; *colTileS = 2; break;
    default:
        *rowTileS = *colTileS = 0; break;
    }
}

// GXFrameBuf.c __GXGetNumXfbLines — verbatim. Given EFB height + the 0x1FF copy scale,
// returns the number of XFB lines produced (clamped to 0x400).
u32 GetNumXfbLines(u32 height, u32 scale) {
#ifdef SMS_NATIVE_PLATFORM
    // Verbatim decomp divides by `scale`; scale==0 (a degenerate render mode whose
    // efbHeight is 0 or whose xfbHeight/efbHeight > 256) would SIGFPE. Name the cause.
    if (scale == 0)
        OSPanic(__FILE__, __LINE__,
                "GetNumXfbLines: copy scale==0 (height=%u) — degenerate render mode "
                "(efbHeight 0 or xfbHeight>>efbHeight upstream)", height);
#endif
    u32 numLines = (height - 1) * 0x100;
    u32 actualHeight = (numLines / scale) + 1;
    u32 newScale = scale;
    if (newScale > 0x80 && newScale < 0x100) {
        while (newScale % 2 == 0) newScale /= 2;
        if (height % newScale == 0) actualHeight++;
    }
    if (actualHeight > 0x400) actualHeight = 0x400;
    return actualHeight;
}

} // namespace

extern "C" {

// GXTexture.c GXGetTexBufferSize — encoded texture byte size (verbatim). __cntlzw is
// only used by the (unused-by-us) mipmap==1 nx seed; we keep the faithful per-level loop.
u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, u8 mipmap, u8 max_lod) {
    u32 tileShiftX, tileShiftY, tileBytes, bufferSize, nx, ny, level;
    GetTexTileShift(format, &tileShiftX, &tileShiftY);
    tileBytes = (format == GX_TF_RGBA8 || format == GX_TF_Z24X8) ? 64u : 32u;
    if (mipmap == 1) {
        bufferSize = 0;
        for (level = 0; level < max_lod; level++) {
            nx = (width  + (1u << tileShiftX) - 1) >> tileShiftX;
            ny = (height + (1u << tileShiftY) - 1) >> tileShiftY;
            bufferSize += tileBytes * (nx * ny);
            if (width == 1 && height == 1) break;
            width  = (width  > 1) ? (u16)(width  >> 1) : 1;
            height = (height > 1) ? (u16)(height >> 1) : 1;
        }
    } else {
        nx = (width  + (1u << tileShiftX) - 1) >> tileShiftX;
        ny = (height + (1u << tileShiftY) - 1) >> tileShiftY;
        bufferSize = nx * ny * tileBytes;
    }
    return bufferSize;
}

u16 GXGetNumXfbLines(u16 efbHeight, f32 yScale) {
    u32 scale = (u32)(256.0f / yScale) & 0x1FF;
    return (u16)GetNumXfbLines(efbHeight, scale);
}

// GXFrameBuf.c GXGetYScaleFactor — verbatim binary-search for the exact XFB fit.
f32 GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight) {
#ifdef SMS_NATIVE_PLATFORM
    if (efbHeight == 0)
        OSPanic(__FILE__, __LINE__,
                "GXGetYScaleFactor: efbHeight==0 (xfbHeight=%u) — render mode object "
                "is zero/uninitialized at the copy-disp site", (u32)xfbHeight);
#endif
    u32 height1 = xfbHeight;
    f32 scale1  = (f32)xfbHeight / (f32)efbHeight;
    u32 scale   = (u32)(256.0f / scale1) & 0x1FF;
    u32 height2 = GetNumXfbLines(efbHeight, scale);
    while (height2 > xfbHeight) {
        height1--;
        scale1  = (f32)height1 / (f32)efbHeight;
        scale   = (u32)(256.0f / scale1) & 0x1FF;
        height2 = GetNumXfbLines(efbHeight, scale);
    }
    f32 scale2 = scale1;
    while (height2 < xfbHeight) {
        scale2 = scale1;
        height1++;
        scale1  = (f32)height1 / (f32)efbHeight;
        scale   = (u32)(256.0f / scale1) & 0x1FF;
        height2 = GetNumXfbLines(efbHeight, scale);
    }
    return scale2;
}

// ===========================================================================
// Display / texture copy configuration (record into GXState).
// ===========================================================================
void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
    auto& d = state().dispCopy;
    d.srcLeft = left; d.srcTop = top; d.srcWd = wd; d.srcHt = ht;
}
void GXSetDispCopyDst(u16 wd, u16 ht) { state().dispCopy.dstWd = wd; state().dispCopy.dstHt = ht; }
void GXSetDispCopyGamma(GXGamma gamma) { state().dispCopy.gamma = gamma; }
void GXSetDispCopyFrame2Field(GXCopyMode mode) { state().dispCopy.frame2field = mode; }
void GXSetCopyClamp(GXFBClamp clamp) { state().dispCopy.clamp = clamp; }
void GXSetCopyFilter(GXBool aa, const u8 /*sample_pattern*/[12][2], GXBool vf,
                     const u8 /*vfilter*/[7]) {
    state().dispCopy.aa = aa; state().dispCopy.vf = vf;
}
// GXSetDispCopyYScale — faithful: derive the 0x1FF copy scale from vertScale and return
// the XFB-line count from the recorded EFB source height (decomp reads gx->cpDispSize,
// set by GXSetDispCopySrc; we read dispCopy.srcHt). Records yScale + the returned lines.
u32 GXSetDispCopyYScale(f32 vertScale) {
    u32 scale = (u32)(256.0f / vertScale) & 0x1FF;
    u32 height = state().dispCopy.srcHt ? state().dispCopy.srcHt : 1;
    u32 lines = GetNumXfbLines(height, scale);
    state().dispCopy.yScale = vertScale;
    state().dispCopy.yScaleLines = lines;
    return lines;
}
// GXCopyDisp/GXCopyTex — the actual EFB->XFB / EFB->texture blit is the native present
// path's job (ngx); in this base lib we record the destination + clear request so the
// caller links and a present layer can consume it. No pixel work here.
void GXCopyDisp(void* dest, GXBool /*clear*/) { state().dispCopy.lastDest = dest; }
void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
    auto& t = state().texCopy; t.srcLeft = left; t.srcTop = top; t.srcWd = wd; t.srcHt = ht;
}
void GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap) {
    auto& t = state().texCopy; t.dstWd = wd; t.dstHt = ht; t.fmt = fmt; t.mipmap = mipmap;
}
// EFB→texture copy. On GC this snapshots the current EFB into the texture at `dest` and (if clear)
// wipes the EFB for the next pass; SMS uses it for the file-select pre-pass + soft-focus composite.
// Record the copy as a render-target boundary in the scene capture (sb_boot_capture_efb_copy, a
// no-op when the capture TU is inactive) so the native present can honor the clear and let a later
// EFB-sampler batch (texmap src == dest) sample the snapshot instead of default-white. See
// debug_journal/2026-06-30_fileselect_overbright_is_efb_target_structure.md.
extern "C" void sb_boot_capture_efb_copy(const void* dest, int clear, int wd, int ht) __attribute__((weak));
void GXCopyTex(void* dest, GXBool clear) {
    state().texCopy.lastDest = dest;
    if (sb_boot_capture_efb_copy)
        sb_boot_capture_efb_copy(dest, clear ? 1 : 0, state().texCopy.dstWd, state().texCopy.dstHt);
}

// ===========================================================================
// Pixel-format / fog / z-texture / alpha (record into GXState).
// ===========================================================================
void GXSetPixelFmt(GXPixelFmt pix_fmt, GXZFmt16 z_fmt) {
    state().pixFmt = pix_fmt; state().zFmt = z_fmt;
}
void GXSetFieldMode(GXBool field_mode, GXBool half_aspect_ratio) {
    state().fieldMode = field_mode; state().halfAspect = half_aspect_ratio;
}
void GXSetAlphaUpdate(GXBool update_enable) { state().alphaUpdate = update_enable; }
void GXSetDstAlpha(GXBool enable, u8 alpha) {
    state().dstAlphaEnable = enable; state().dstAlpha = alpha;
}
void GXSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    auto& f = state().fog;
    f.type = type; f.startz = startz; f.endz = endz; f.nearz = nearz; f.farz = farz;
    f.color = color;
}
// GXSetFogRangeAdj — the per-x fog range-adjust LUT modifies fog falloff at screen
// edges; the native fog path doesn't model the edge-correction table -> documented
// no-op (the base fog params from GXSetFog are the faithful state we keep).
void GXSetFogRangeAdj(GXBool /*enable*/, u16 /*center*/, GXFogAdjTable* /*table*/) {}
void GXSetZTexture(GXZTexOp op, GXTexFmt fmt, u32 bias) {
    auto& z = state().zTex; z.op = op; z.fmt = fmt; z.bias = bias;
}

// ===========================================================================
// Indirect-texture ordering (record); warp is the indirect TEV path (parked).
// ===========================================================================
void GXSetIndTexOrder(GXIndTexStageID ind_stage, GXTexCoordID tex_coord, GXTexMapID tex_map) {
    if ((u32)ind_stage >= 4) return;
    auto& o = state().indOrder[ind_stage];
    o.texCoord = (u8)tex_coord; o.texMap = (u8)(tex_map & ~0x100);
}
void GXSetTevIndWarp(GXTevStageID /*tev_stage*/, GXIndTexStageID /*ind_stage*/,
                     u8 /*signed_offset*/, u8 /*replace_mode*/, GXIndTexMtxID /*matrix_sel*/) {
    /* indirect-warp configures the parked indirect-TEV draw path; no native state yet */
}

// ===========================================================================
// Vertex attribute format (vector form) — loops the list onto GXSetVtxAttrFmt.
// ===========================================================================
void GXSetVtxAttrFmtv(GXVtxFmt vtxfmt, const GXVtxAttrFmtList* list) {
    for (const GXVtxAttrFmtList* e = list; e && e->attr != GX_VA_NULL; ++e)
        GXSetVtxAttrFmt(vtxfmt, e->attr, e->cnt, e->type, e->frac);
}

// ===========================================================================
// Palettes (TLUT) — native overlay on the opaque 12-byte GXTlutObj.
// ===========================================================================
// The opaque GXTlutObj is only 12 bytes — too small to hold an 8-byte host pointer plus
// metadata. So the obj carries just {magic, registry slot}; the lut pointer + fmt + count
// live in a fixed side-registry (round-robin, bounded). GXLoadTlut copies the registered
// descriptor into the bound-palette slot the renderer reads.
namespace {
struct NativeTlut { const void* lut; u16 nEntries; u8 fmt; };
struct TlutObjOverlay { u32 magic; u32 slot; };
static_assert(sizeof(TlutObjOverlay) <= sizeof(GXTlutObj), "tlut obj overlay fits");
const u32 kTlutMagic = 0x544C5530; // 'TLU0'
constexpr int kTlutRegN = 64;
NativeTlut g_tlutReg[kTlutRegN]{};
int g_tlutNext = 0;
} // namespace
void GXInitTlutObj(GXTlutObj* tlut_obj, void* lut, GXTlutFmt fmt, u16 n_entries) {
    auto* t = reinterpret_cast<TlutObjOverlay*>(tlut_obj);
    int slot = g_tlutNext; g_tlutNext = (g_tlutNext + 1) % kTlutRegN;
    g_tlutReg[slot] = { lut, n_entries, (u8)fmt };
    t->magic = kTlutMagic; t->slot = (u32)slot;
}
void GXLoadTlut(GXTlutObj* tlut_obj, u32 tlut_name) {
    if (tlut_name >= 20) return;
    auto* t = reinterpret_cast<TlutObjOverlay*>(tlut_obj);
    auto& b = state().tlut[tlut_name];
    if (t->magic != kTlutMagic || t->slot >= (u32)kTlutRegN) { b.valid = false; return; }
    const NativeTlut& r = g_tlutReg[t->slot];
    b = { r.lut, r.nEntries, r.fmt, true };
}
// No TMEM on the native renderer; textures sample host images directly.
void GXInvalidateTexAll(void) {}

// ===========================================================================
// Draw-sync / management (record token + callback; no FIFO to flush).
// ===========================================================================
void GXFlush(void) {}              // no command FIFO to flush
void GXSetDrawDone(void) {}        // drawsync barrier — handled by the present path
void GXWaitDrawDone(void) {}       // synchronous native renderer; nothing to wait on
void GXSetDrawSync(u16 token) { state().drawSyncToken = token; }
GXDrawSyncCallback GXSetDrawSyncCallback(GXDrawSyncCallback cb) {
    GXDrawSyncCallback prev = state().drawSyncCb;
    state().drawSyncCb = cb;
    return prev;
}
void GXEnableBreakPt(void* /*break_pt*/) {} // GP-FIFO breakpoints — no FIFO natively
void GXDisableBreakPt(void) {}
GXFifoObj* GXGetCPUFifo(void) { static GXFifoObj fifo{}; return &fifo; }
void GXGetFifoPtrs(GXFifoObj* /*fifo*/, void** readPtr, void** writePtr) {
    if (readPtr)  *readPtr  = nullptr;   // no FIFO ring in the native renderer
    if (writePtr) *writePtr = nullptr;
}

// ===========================================================================
// EFB peek / perf metrics — no EFB/perf counters in this base lib. The native
// renderer provides depth/colour occlusion via its own overrides (see ngx); here
// they return cleared values so boot/loader code links and reads a defined result.
// ===========================================================================
void GXPeekARGB(u16 /*x*/, u16 /*y*/, u32* color) { if (color) *color = 0; }
void GXPeekZ(u16 /*x*/, u16 /*y*/, u32* z) { if (z) *z = 0; }
void GXPokeAlphaRead(GXAlphaReadMode /*mode*/) {}
void GXClearPixMetric(void) {}
void GXReadPixMetric(u32* a, u32* b, u32* c, u32* d, u32* e, u32* f) {
    if (a)*a=0; if(b)*b=0; if(c)*c=0; if(d)*d=0; if(e)*e=0; if(f)*f=0;
}

// ===========================================================================
// Misc HW raster knobs with no native counterpart — documented no-ops.
// ===========================================================================
void GXSetPointSize(u8 /*pointSize*/, GXTexOffset /*texOffsets*/) {}
void GXEnableTexOffsets(GXTexCoordID /*coord*/, u8 /*line*/, u8 /*point*/) {}
void GXSetTexCoordCylWrap(GXTexCoordID /*coord*/, u8 /*s*/, u8 /*t*/) {}
void GXSetTexCoordScaleManually(GXTexCoordID /*coord*/, u8 /*enable*/, u16 /*ss*/, u16 /*ts*/) {}

// ===========================================================================
// Immediate-mode draw verbs. GXDrawSphere is the TSky procedural backdrop dome — route it to the
// native scene capture (sms_boot_j3d_capture.cpp), which replicates the sphere, transforms it by
// the current pos matrix, colours it with the matColor, clips, and emits a depth-tested batch.
// WEAK so the base lib still links where the capture TU isn't present (tests). GXDrawCube (the
// Mario occlusion probe) stays inert here — handled natively via depth-occlusion, not geometry.
// ===========================================================================
extern "C" void sb_boot_capture_sphere(int numMajor, int numMinor) __attribute__((weak));
void GXDrawCube(void) {}
void GXDrawSphere(u8 numMajor, u8 numMinor) {
    if (&sb_boot_capture_sphere) sb_boot_capture_sphere((int)numMajor, (int)numMinor);
}

// ===========================================================================
// GXInit — create the native GX context (defaults already in GXState's ctor) and hand
// back a FIFO object the caller can hold. No GP-FIFO is allocated; the renderer reads
// GXState directly.
// ===========================================================================
GXFifoObj* GXInit(void* /*base*/, u32 /*size*/) {
    (void)state();              // force the GXState singleton up with its defaults
    static GXFifoObj fifo{};
    return &fifo;
}

} // extern "C"

// ---------------------------------------------------------------------------
// NTSC 480i render-mode data symbol — verbatim from the decomp (GXFrameBuf.c).
// Referenced by the boot/exception path (&GXNtsc480Int). Not inside extern "C" wrapping
// above to keep it a plain C-linkage object; declared `extern GXRenderModeObj` in the
// SDK header, so this provides the definition.
// ---------------------------------------------------------------------------
extern "C" {
GXRenderModeObj GXNtsc480Int = {
    (VITVMode)0, 640, 480, 480, 40, 0, 640, 480, (VIXFBMode)1, 0, 0,
    { {6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6} },
    { 0, 0, 21, 22, 21, 0, 0 },
};
}
