// gx_impl.cpp — native GX seam, SLICE 1: the transform block (projection/viewport/
// scissor + GXProject). "Rebuild as a PC game": GXSet* capture into the native GXState
// (gx_state.h); the GameCube FIFO / XF-register writes the decomp does are DROPPED —
// the native renderer reads GXState, there is no command stream to emulate.
//
// GXProject is ported verbatim from the decomp (GXTransform.c) — pure eye->screen math.
// GXSetProjection/GXSetViewport/GXSetScissor port only the gx-struct writes (not the
// GX_WRITE_* FIFO pokes). GXGetProjectionv/GXGetViewportv read the state back in the
// exact pm[]/vp[] layout GXProject expects. The full GX surface (TEV/blend/Z/tex/draw/
// present) lands in later slices, routed to the ngx renderer.

#include <dolphin/gx.h>
#include "gx_state.h"

namespace sb::platform::gx {
GXState& state() { static GXState g_gx{}; return g_gx; }
}

using sb::platform::gx::state;

extern "C" {

// Eye->screen projection (verbatim from the decomp; self-contained pure math).
void GXProject(f32 x, f32 y, f32 z, f32 mtx[3][4], f32* pm, f32* vp,
               f32* sx, f32* sy, f32* sz) {
    f32 ex = mtx[0][3] + ((mtx[0][2]*z) + ((mtx[0][0]*x) + (mtx[0][1]*y)));
    f32 ey = mtx[1][3] + ((mtx[1][2]*z) + ((mtx[1][0]*x) + (mtx[1][1]*y)));
    f32 ez = mtx[2][3] + ((mtx[2][2]*z) + ((mtx[2][0]*x) + (mtx[2][1]*y)));
    f32 xc, yc, zc, wc;
    if (pm[0] == 0.0f) {           // perspective (pm[0] = GX_PERSPECTIVE = 0)
        xc = (ex*pm[1]) + (ez*pm[2]);
        yc = (ey*pm[3]) + (ez*pm[4]);
        zc = pm[6] + (ez*pm[5]);
        wc = 1.0f / -ez;
    } else {                        // orthographic
        xc = pm[2] + (ex*pm[1]);
        yc = pm[4] + (ey*pm[3]);
        zc = pm[6] + (ez*pm[5]);
        wc = 1.0f;
    }
    *sx = (vp[2]/2.0f) + (vp[0] + (wc * (xc * vp[2]/2.0f)));
    *sy = (vp[3]/2.0f) + (vp[1] + (wc * (-yc * vp[3]/2.0f)));
    *sz = vp[5] + (wc * (zc * (vp[5] - vp[4])));
}

void GXSetProjection(f32 mtx[4][4], GXProjectionType type) {
    auto& g = state();
    g.projType   = type;
    g.projMtx[0] = mtx[0][0];
    g.projMtx[2] = mtx[1][1];
    g.projMtx[4] = mtx[2][2];
    g.projMtx[5] = mtx[2][3];
    if (type == GX_ORTHOGRAPHIC) {
        g.projMtx[1] = mtx[0][3];
        g.projMtx[3] = mtx[1][3];
    } else {
        g.projMtx[1] = mtx[0][2];
        g.projMtx[3] = mtx[1][2];
    }
}

void GXGetProjectionv(f32* ptr) {
    auto& g = state();
    ptr[0] = (f32)g.projType;
    ptr[1] = g.projMtx[0];
    ptr[2] = g.projMtx[1];
    ptr[3] = g.projMtx[2];
    ptr[4] = g.projMtx[3];
    ptr[5] = g.projMtx[4];
    ptr[6] = g.projMtx[5];
}

void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz,
                         u32 field) {
    if (field == 0) top -= 0.5f;   // matches the decomp (de-jitter for field 0)
    auto& g = state();
    g.vpLeft = left; g.vpTop = top; g.vpWd = wd; g.vpHt = ht;
    g.vpNearz = nearz; g.vpFarz = farz;
}

void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
    GXSetViewportJitter(left, top, wd, ht, nearz, farz, 1u);
}

void GXGetViewportv(f32* vp) {
    auto& g = state();
    vp[0] = g.vpLeft; vp[1] = g.vpTop; vp[2] = g.vpWd; vp[3] = g.vpHt;
    vp[4] = g.vpNearz; vp[5] = g.vpFarz;
}

void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht) {
    auto& g = state();
    g.scLeft = left; g.scTop = top; g.scWd = wd; g.scHt = ht;
}

// --- SLICE 2: core pixel-pipeline state -----------------------------------
// Capture clean semantic values into GXState (no GC BP register bit-packing) — the
// native renderer maps these to its pipeline directly.
void GXSetBlendMode(GXBlendMode type, GXBlendFactor src, GXBlendFactor dst, GXLogicOp op) {
    auto& g = state();
    g.blendType = type; g.blendSrc = src; g.blendDst = dst; g.blendLogicOp = op;
}
void GXSetZMode(GXBool compare, GXCompare func, GXBool update) {
    auto& g = state();
    g.zCompare = compare; g.zFunc = func; g.zUpdate = update;
}
void GXSetZCompLoc(GXBool beforeTex) { state().zCompLocBeforeTex = beforeTex; }
void GXSetCullMode(GXCullMode mode) { state().cullMode = mode; }
void GXSetColorUpdate(GXBool enable) { state().colorUpdate = enable; }
void GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    auto& g = state();
    g.alphaComp0 = comp0; g.alphaRef0 = ref0; g.alphaOp = op;
    g.alphaComp1 = comp1; g.alphaRef1 = ref1;
}
// GXColor passed BY VALUE per the SDK header — the native compiler handles the ABI
// (the "GXColor by pointer" caveat was a RECOMP/MWcc artifact, irrelevant here).
void GXSetCopyClear(GXColor clear, u32 z) {
    auto& g = state();
    g.copyClearColor = clear; g.copyClearZ = z;
}
void GXSetNumChans(u8 n)     { state().numChans = n; }
void GXSetNumTexGens(u8 n)   { state().numTexGens = n; }
void GXSetNumTevStages(u8 n) { state().numTevStages = n; }

// --- SLICE 3: lighting (chan-ctrl/material/ambient + light objects) ---------
// GXSet* capture into GXState; the native renderer (ngx_light) consumes them per
// vertex. "Rebuild as a PC game": the GXLightObj is OPAQUE on hardware (16 HW
// register words), so we own its packing — overlaying it with a friendly float
// layout that GXLoadLightObjImm copies into GXState. No FIFO/XF register writes.

namespace {
// Map a GXChannelID to the two GXState.chan slots it writes. COLOR/ALPHA channels
// write one slot; the combined COLOR0A0/COLOR1A1 write the colour+alpha pair.
// Returns the count (1 or 2) and fills idx[0..count-1].
int chan_slots(GXChannelID chan, int idx[2]) {
    switch (chan) {
    case GX_COLOR0:   idx[0] = 0; return 1;
    case GX_COLOR1:   idx[0] = 1; return 1;
    case GX_ALPHA0:   idx[0] = 2; return 1;
    case GX_ALPHA1:   idx[0] = 3; return 1;
    case GX_COLOR0A0: idx[0] = 0; idx[1] = 2; return 2;
    case GX_COLOR1A1: idx[0] = 1; idx[1] = 3; return 2;
    default:          idx[0] = 0; return 1;
    }
}

// GXLightID is a single-bit mask (GX_LIGHT0=0x1 .. GX_LIGHT7=0x80) -> 0..7 index.
int light_index(GXLightID id) {
    unsigned m = (unsigned)id;
    for (int i = 0; i < 8; ++i) if (m & (1u << i)) return i;
    return 0;
}

// The native overlay of the opaque 64-byte GXLightObj (16 words = 16 floats).
struct NativeLightObj {
    f32 color[4];     // 16
    f32 pos[3];       // 12
    f32 dir[3];       // 12
    f32 cosAtt[3];    // 12  angle attenuation a0,a1,a2
    f32 distAtt[3];   // 12  distance attenuation k0,k1,k2
};
static_assert(sizeof(NativeLightObj) <= sizeof(GXLightObj), "light obj overlay fits");
NativeLightObj& obj(GXLightObj* o) { return *reinterpret_cast<NativeLightObj*>(o); }
} // namespace

void GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src,
                   u32 light_mask, GXDiffuseFn diff_fn, GXAttnFn attn_fn) {
    // Pack into the decode_chanctl() layout (b0 matSrc, b1 enable, b2-5 mask lo,
    // b6 ambSrc, b7-8 diffFn, b9-10 attnFn, b11-14 mask hi). attnFn packs as the GC
    // 2-bit field: NONE->0, SPEC->1 (enable, select=spec), SPOT->3 (enable, select=spot).
    u32 attn = (attn_fn == GX_AF_SPEC) ? 1u : (attn_fn == GX_AF_SPOT) ? 3u : 0u;
    u32 cc = ((u32)(mat_src & 1) << 0) | ((u32)(enable & 1) << 1)
           | (((u32)light_mask & 0x0F) << 2) | ((u32)(amb_src & 1) << 6)
           | (((u32)diff_fn & 3) << 7) | (attn << 9)
           | ((((u32)light_mask >> 4) & 0x0F) << 11);
    auto& g = state();
    int idx[2]; int n = chan_slots(chan, idx);
    for (int i = 0; i < n; ++i) g.chan[idx[i]].ctrl = cc;
}

void GXSetChanMatColor(GXChannelID chan, GXColor mat_color) {
    auto& g = state();
    int idx[2]; int n = chan_slots(chan, idx);
    for (int i = 0; i < n; ++i) g.chan[idx[i]].matColor = mat_color;
}

void GXSetChanAmbColor(GXChannelID chan, GXColor amb_color) {
    auto& g = state();
    int idx[2]; int n = chan_slots(chan, idx);
    for (int i = 0; i < n; ++i) g.chan[idx[i]].ambColor = amb_color;
}

void GXInitLightColor(GXLightObj* lt, GXColor color) {
    auto& o = obj(lt);
    o.color[0] = color.r / 255.f; o.color[1] = color.g / 255.f;
    o.color[2] = color.b / 255.f; o.color[3] = color.a / 255.f;
}
void GXInitLightPos(GXLightObj* lt, f32 x, f32 y, f32 z) {
    auto& o = obj(lt); o.pos[0] = x; o.pos[1] = y; o.pos[2] = z;
}
void GXInitLightDir(GXLightObj* lt, f32 nx, f32 ny, f32 nz) {
    // GX stores the NEGATED direction (light-to-vertex); the decomp/HW negate here.
    auto& o = obj(lt); o.dir[0] = -nx; o.dir[1] = -ny; o.dir[2] = -nz;
}
void GXInitSpecularDir(GXLightObj* lt, f32 nx, f32 ny, f32 nz) {
    auto& o = obj(lt); o.dir[0] = nx; o.dir[1] = ny; o.dir[2] = nz;
}
void GXInitLightAttn(GXLightObj* lt, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2) {
    auto& o = obj(lt);
    o.cosAtt[0]=a0; o.cosAtt[1]=a1; o.cosAtt[2]=a2;
    o.distAtt[0]=k0; o.distAtt[1]=k1; o.distAtt[2]=k2;
}
void GXInitLightAttnA(GXLightObj* lt, f32 a0, f32 a1, f32 a2) {
    auto& o = obj(lt); o.cosAtt[0]=a0; o.cosAtt[1]=a1; o.cosAtt[2]=a2;
}
void GXInitLightAttnK(GXLightObj* lt, f32 k0, f32 k1, f32 k2) {
    auto& o = obj(lt); o.distAtt[0]=k0; o.distAtt[1]=k1; o.distAtt[2]=k2;
}

void GXLoadLightObjImm(GXLightObj* lt, GXLightID light) {
    auto& o = obj(lt);
    auto& L = state().light[light_index(light)];
    L.valid = true;
    for (int i = 0; i < 4; ++i) L.color[i] = o.color[i];
    for (int i = 0; i < 3; ++i) { L.pos[i]=o.pos[i]; L.dir[i]=o.dir[i];
                                  L.cosAtt[i]=o.cosAtt[i]; L.distAtt[i]=o.distAtt[i]; }
}

void GXGetLightColor(const GXLightObj* lt, GXColor* color) {
    const auto& o = obj(const_cast<GXLightObj*>(lt));
    color->r = (u8)(o.color[0]*255.f + 0.5f); color->g = (u8)(o.color[1]*255.f + 0.5f);
    color->b = (u8)(o.color[2]*255.f + 0.5f); color->a = (u8)(o.color[3]*255.f + 0.5f);
}

// --- SLICE 4: TEV combiner state (GXSetTev*) --------------------------------
// Ported from the decomp GXTev.c, but writing into GXState.tev (the BP-register bit
// layout = NgxTevState.color_env/alpha_env) instead of the GC FIFO. The renderer's
// tev_shader decodes this directly (see gx_tev_bridge.h ngx_tevstate_from_gx).
namespace {
// GXTev.c SET_REG_FIELD(reg, size, shift, val): replace `size` bits at `shift`.
inline void set_field(u32& reg, int size, int shift, u32 val) {
    const u32 mask = ((1u << size) - 1u) << shift;
    reg = (reg & ~mask) | ((val << shift) & mask);
}
} // namespace

void GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b,
                     GXTevColorArg c, GXTevColorArg d) {
    u32& r = state().tev.colorEnv[stage];
    set_field(r, 4, 12, a); set_field(r, 4, 8, b); set_field(r, 4, 4, c); set_field(r, 4, 0, d);
}
void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b,
                     GXTevAlphaArg c, GXTevAlphaArg d) {
    u32& r = state().tev.alphaEnv[stage];
    set_field(r, 3, 13, a); set_field(r, 3, 10, b); set_field(r, 3, 7, c); set_field(r, 3, 4, d);
}
void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale,
                     GXBool clamp, GXTevRegID out_reg) {
    u32& r = state().tev.colorEnv[stage];
    set_field(r, 1, 18, op & 1);
    if (op <= 1) { set_field(r, 2, 20, scale); set_field(r, 2, 16, bias); }
    else         { set_field(r, 2, 20, (op >> 1) & 3); set_field(r, 2, 16, 3); }
    set_field(r, 1, 19, clamp & 0xFF); set_field(r, 2, 22, out_reg);
}
void GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale,
                     GXBool clamp, GXTevRegID out_reg) {
    u32& r = state().tev.alphaEnv[stage];
    set_field(r, 1, 18, op & 1);
    if (op <= 1) { set_field(r, 2, 20, scale); set_field(r, 2, 16, bias); }
    else         { set_field(r, 2, 20, (op >> 1) & 3); set_field(r, 2, 16, 3); }
    set_field(r, 1, 19, clamp & 0xFF); set_field(r, 2, 22, out_reg);
}
void GXSetTevOp(GXTevStageID id, GXTevMode mode) {
    GXTevColorArg carg = (id != GX_TEVSTAGE0) ? GX_CC_CPREV : GX_CC_RASC;
    GXTevAlphaArg aarg = (id != GX_TEVSTAGE0) ? GX_CA_APREV : GX_CA_RASA;
    switch (mode) {
    case GX_MODULATE:
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_TEXC, carg, GX_CC_ZERO);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_TEXA, aarg, GX_CA_ZERO); break;
    case GX_DECAL:
        GXSetTevColorIn(id, carg, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, aarg); break;
    case GX_BLEND:
        GXSetTevColorIn(id, carg, GX_CC_ONE, GX_CC_TEXC, GX_CC_ZERO);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_TEXA, aarg, GX_CA_ZERO); break;
    case GX_REPLACE:
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA); break;
    case GX_PASSCLR:
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, carg);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, aarg); break;
    default: break;
    }
    GXSetTevColorOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}
void GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID color) {
    auto& t = state().tev;
    u32 tmap = map & ~0x100;
    t.texmap[stage]   = (tmap >= GX_MAX_TEXMAP) ? 0xff : (u8)tmap;
    t.texcoord[stage] = (coord >= GX_MAX_TEXCOORD) ? 0xff : (u8)coord;
    t.colorChan[stage] = (u8)color;     // raw GXChannelID (shader maps COLOR0/COLOR1)
}
void GXSetTevColor(GXTevRegID id, GXColor color) {
    auto& c = state().tev.tevColor[id];
    c[0] = color.r; c[1] = color.g; c[2] = color.b; c[3] = color.a;
}
void GXSetTevColorS10(GXTevRegID id, GXColorS10 color) {
    auto& c = state().tev.tevColor[id];
    c[0] = color.r; c[1] = color.g; c[2] = color.b; c[3] = color.a;
}
void GXSetTevKColor(GXTevKColorID id, GXColor color) {
    auto& k = state().tev.kColor[id];
    k[0] = color.r; k[1] = color.g; k[2] = color.b; k[3] = color.a;
}
void GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel sel) { state().tev.kcsel[stage] = (u8)sel; }
void GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel sel) { state().tev.kasel[stage] = (u8)sel; }
void GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel ras_sel, GXTevSwapSel tex_sel) {
    // The renderer reads swap_table[alphaEnv&3] (raster) and [(alphaEnv>>2)&3] (texture).
    u32& r = state().tev.alphaEnv[stage];
    set_field(r, 2, 0, ras_sel); set_field(r, 2, 2, tex_sel);
}
void GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green,
                           GXTevColorChan blue, GXTevColorChan alpha) {
    // NgxTevState swizzle byte: r=(b>>6)&3 g=(b>>4)&3 b=(b>>2)&3 a=b&3.
    state().tev.swapTable[table] = (u8)(((red & 3) << 6) | ((green & 3) << 4) |
                                        ((blue & 3) << 2) | (alpha & 3));
}
void GXSetTevDirect(GXTevStageID /*stage*/) { /* no indirect captured: stage is direct */ }
void GXSetNumIndStages(u8 n) { state().tev.numIndStages = n; }

} // extern "C"
