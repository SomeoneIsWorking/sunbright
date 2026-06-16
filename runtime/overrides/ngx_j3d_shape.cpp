// ngx_j3d_shape — N4 live hook (docs/native_port_plan.md §3): observe the game's
// J3D world-geometry draws and assemble them into native meshes, Dolphin-free.
//
// A tee on J3DShape::draw (0x802e0390) reads the shape's vertex descriptor from
// the J3D OBJECTS (not by decoding a GX byte stream): GXVtxDescList (unk2C) →
// VCD, J3DVertexData::getVtxAttrFmtList (unk44+0xC) → VAT, and the live vertex
// arrays (j3dSys pos/nrm/clr0 override + J3DVertexData for clr1/tex). It then
// walks each J3DShapeDraw display list (mDraws[i]->mDisplayList — the GX
// primitive stream) through ngx_build_mesh, producing native vertices + a
// triangle-index list per shape. The real recompiled draw still runs (Dolphin
// keeps rasterizing during bring-up); we only OBSERVE, and publish stats over the
// probe (/ngxshape) so the native geometry can be verified vs the oracle.
//
// Layout (reference/sms JSystem decomp, verified addresses):
//   J3DShape:     mElementCount@0x6, mGDCommands@0x28, GXVtxDescList* unk2C@0x2C,
//                 bool unk30(NBT)@0x30, J3DShapeDraw** mDraws@0x38,
//                 J3DVertexData* unk44@0x44.
//   J3DShapeDraw: vtable@0, mDisplayListSize@4, const u8* mDisplayList@8 (the
//                 decomp header omits the vtable; confirmed by disasm of the ctor
//                 0x802dfe70 — stw r4(list),8(this) / stw r5(size),4(this) — and
//                 draw 0x802dfe88 — lwz list,8(this) / lwz size,4(this)).
//   J3DVertexData: GXVtxAttrFmtList* @0xC, posArr@0x10, nrmArr@0x14, nbtArr@0x18,
//                  colorArr[2]@0x1C, texCoordArr[8]@0x24.
//   j3dSys @0x804045DC: unk10C(vtxPos)@+0x10C, unk110(vtxNrm)@+0x110,
//                  unk114(vtxClr0)@+0x114  (loadVtxArray's live-buffer override).
//   GXVtxDescList entry = {u32 attr, u32 type} (8 B). GXVtxAttrFmtList entry =
//                  {u32 attr, u32 cnt, u32 type, u8 frac} (16 B). attr 0xFF = end.
//   GXAttr: POS=9 NRM=10 CLR0=11 CLR1=12 TEX0..7=13..20. GXAttrType=VCD class.

#include "../overrides.h"
#include "../intrinsics.h"
#include "../ngx/ngx_mesh.h"
#include "../ngx/ngx_render_data.h"
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <atomic>
#include <vector>
#include <algorithm>
#include "VideoCommon/XFMemory.h"   // ground-truth GX lighting state (Dolphin XF registers)
#include "VideoCommon/BPMemory.h"   // ground-truth GX pixel state (fog / blend / etc.)
#include "VideoCommon/CPMemory.h"   // ground-truth GX vertex array bases (g_main_cp_state)
#include "../render/tex_decode.h"   // DBG: decode a texture to check its brightness

namespace {

constexpr u32 J3DSYS = 0x804045DCu;
// Honor the VALUE (=0 disables) so a recomp-GX oracle with NGX_PRESENT=0 truly runs Dolphin GX.
static bool sb_env_on(const char* n) { const char* v = getenv(n); return v && atoi(v) != 0; }
bool g_enabled = sb_env_on("SUNBRIGHT_NGX_SHAPE") || sb_env_on("SUNBRIGHT_NGX_PRESENT");  // present needs capture

// Latest GX texmap-0 binding (from the GXLoadTexObj tee), associated with shapes
// drawn after it. Decoded from the GXTexObj's packed registers.
struct CurTex { u32 addr = 0; u16 w = 0, h = 0; u8 fmt = 0; u32 tlut_addr = 0; u8 tlut_fmt = 0; bool valid = false; };
CurTex g_curtex[8];   // live binding per GX texmap (0..7) — GX-tee path (diagnostic only)

// N6.7: the per-material texture binding read OBJECT-MODEL (J3D preloads textures
// into TMEM upfront, so the GXLoadTexObj tee is stale at draw time — the real
// per-shape texture is selected by the material). Filled by capture_textures.
NgxTexBind g_mat_tex[8];

// TLUT registry: tlut_name (GXTlut, the TMEM slot) → palette guest addr + format,
// populated by the GXLoadTlut tee. CI texobjs reference a tlut_name (texobj+0x18).
struct TlutEntry { u32 addr = 0; u8 fmt = 0; bool valid = false; };
TlutEntry g_tlut[256];

inline bool valid(u32 a) { return a >= 0x80000000u && a < 0x81800000u; }
inline u32  r32(u32 a) { return valid(a) ? mem_r32(a) : 0; }
inline u8   rb(u32 a) { return valid(a) ? (u8)(mem_r32(a) >> 24) : 0; }  // big-endian byte @ word
inline float rf(u32 a) { u32 u = r32(a); float f; std::memcpy(&f, &u, 4); return f; }

// Decode a GXTexObj's packed registers (image0@0x08 = (w-1)[0:9]/(h-1)[10:19]/
// fmt&0xF[20:23]; image3@0x0C = (addr>>5)[0:20]; tlutName@0x18 for CI) into a
// CurTex. Shared by the GXLoadTexObj and GXLoadTexObjPreLoaded tees.
void decode_texobj(u32 obj, CurTex& t) {
    const u32 image0 = r32(obj + 0x08), image3 = r32(obj + 0x0C);
    t.w = (u16)((image0 & 0x3FF) + 1);
    t.h = (u16)(((image0 >> 10) & 0x3FF) + 1);
    t.fmt = (u8)((image0 >> 20) & 0xF);
    t.addr = 0x80000000u | ((image3 & 0x1FFFFFu) << 5);
    t.tlut_addr = 0; t.tlut_fmt = 0;
    if (t.fmt == 0x8 || t.fmt == 0x9 || t.fmt == 0xA) {        // CI → resolve TLUT
        const u32 name = r32(obj + 0x18) & 0xFF;               // texobj.tlutName
        if (g_tlut[name].valid) { t.tlut_addr = g_tlut[name].addr; t.tlut_fmt = g_tlut[name].fmt; }
    }
    t.valid = true;
}

// ── N6 color-channel LIGHTING (docs/native_port_plan.md §6) ─────────────────────
// GX rasterizes a lit "channel colour" per vertex (raster colour input to the TEV),
// not the raw CLR0 attribute, whenever a material's colour channel has lighting on
// (SMS world geometry uses a sun + ambient). We port that stage natively:
//   • capture the 8 hardware lights at GXLoadLightObjImm (the state the GPU uses,
//     already transformed into the position-matrix / eye space by the scene), and
//   • read the material's J3DColorBlock (channel control + material/ambient colour
//     registers), then evaluate the GX lighting equation per vertex in eye space
//     (vertex pos via the modelview, normal via j3dSys.mCurrentNormMtx @ +0x108).
// The result replaces the per-vertex raster colour fed to the TEV shader.
//
// GXLightObj packed layout (verified by disasm of GXInitLightPos/Color/Attn/
// SpecularDir): colour u32@0x0C, cosAtten(a0,a1,a2)@0x10/14/18, distAtten(k0,k1,k2)
// @0x1C/20/24, position@0x28/2C/30, direction(half-angle)@0x34/38/3C.
struct LightObj {
    bool  valid = false;
    float color[3] = {0, 0, 0};   // RGB 0..1
    float pos[3]   = {0, 0, 0};    // eye-space position
    float dir[3]   = {0, 0, 0};    // eye-space (half-angle) direction
    float cosA[3]  = {0, 0, 0};    // angle attenuation a0,a1,a2
    float distA[3] = {0, 0, 0};    // distance attenuation k0,k1,k2
};
LightObj g_light[8];
unsigned long g_light_loads = 0;

// Global hardware ambient colour register per colour channel (0,1), captured at
// GXSetChanAmbColor. J3DColorBlockLightOff blocks do NOT store/load an ambient
// (only LightOn does) — for them the ambient is whatever the scene set globally,
// so we must read the hardware register, not the block.
u8 g_amb_reg[2][4] = {{0,0,0,0}, {0,0,0,0}};
bool g_amb_have[2] = {false, false};
unsigned long g_amb_sets = 0;
// Authoritative per-channel lighting state captured SYNCHRONOUSLY from the GX commands
// (GXSetChanCtrl / GXSetChanMatColor) — the same LitChannel.hex / matColor Dolphin uses.
u16  g_gx_cc[2] = {0, 0};            bool g_gx_cc_have[2] = {false, false};
u8   g_gx_matcol[2][4] = {{255,255,255,255},{255,255,255,255}}; bool g_gx_matcol_have[2] = {false,false};
// DBG histograms for the brightness/wash investigation (vert-weighted).
unsigned long g_clr0cls_hist[4] = {0}, g_matsrc_hist[3] = {0}, g_litcfg_hist[2] = {0};
size_t g_bigmap_verts = 0; unsigned g_bigmap_clr0cls = 0; bool g_bigmap_matvtx = false, g_bigmap_en = false;
u8 g_bigmap_vcol[3] = {0}; u16 g_bigmap_cc = 0;
double g_colcat_sum[5] = {0}; unsigned long g_colcat_n[5] = {0};  // col0 lum by category
double g_uplit_sum=0, g_uplit_max=0, g_uplit_amb=0, g_uplit_ndl=0; unsigned long g_uplit_n=0;
unsigned long g_clr0fmt_hist[8] = {0};  // CLR0 VAT format (0=565,1=888,2=888x,3=4444,4=6666,5=8888)
size_t g_bigany_verts = 0; u16 g_bigany_cc = 0; bool g_bigany_hasnrm=false, g_bigany_matvtx=false, g_bigany_en=false;
unsigned g_bigany_clr0cls=0; u8 g_bigany_vcol[3]={0}; double g_bigany_vcolmean=0; u32 g_bigany_clr0base=0;

// J3DColorBlock variants (vtable ptrs, gmse01; from the block ctor/dtor disasm:
// LightOn sets 0x803E0CD4, LightOff sets 0x803E0D38). Field offsets from
// J3DColorBlocks.hpp — LightOn: mMatColor@0x04, mAmbColor@0x0C, mColorChan@0x16;
// LightOff: mMatColor@0x04, mColorChan@0x0E (no ambient / no lights).
constexpr u32 VT_CLON = 0x803E0CD4u, VT_CLOF = 0x803E0D38u;

// The current material's colour-channel state (set by capture_colorchan, consumed
// by light_vertex during transform_eye for the shape being captured).
struct ChanInfo {
    bool valid = false;
    u16  color0 = 0, alpha0 = 0;          // J3DColorChan COLOR0 / ALPHA0 ctrl regs
    u8   matColor[4] = {255, 255, 255, 255};
    u8   ambColor[4] = {0, 0, 0, 0};
    u8   cullMode = 0;                     // GXCullMode (color block mCullMode): NONE/FRONT/BACK/ALL
};
ChanInfo g_cur_chan;
unsigned long g_chan_lit = 0, g_chan_flat = 0;
u8 g_dbg_cb_raw[0x44] = {0}; bool g_dbg_cb_have = false; u32 g_dbg_cb_vt = 0;
u32 g_cbvt_key[8] = {0}; unsigned g_cbvt_cnt[8] = {0};   // colour-block vtable histogram
// Flat-path breakdown + brightness diagnostics (per-vertex sums; /ngxshape).
unsigned long g_flat_reg = 0, g_flat_vtx = 0;
double g_lit_lum_sum = 0, g_flat_lum_sum = 0;
double g_diff_sum = 0; unsigned long g_diff_n = 0;   // mean diffuse dot over lit verts/lights
// Last lit material's decoded state (snapshot for inspection).
u16 g_dbg_cc = 0; u8 g_dbg_mat[4] = {0}, g_dbg_amb[4] = {0};
float g_dbg_illum[3] = {0}, g_dbg_out[4] = {0};
double g_nrm_len_sum = 0; unsigned long g_nrm_n = 0;   // mean eye-normal length (sanity)
double g_lit_nrm_sum = 0; unsigned long g_lit_nrm_n = 0, g_lit_nrm_zero = 0;  // lit-vert normals
// Light-0 (sun) diffuse alignment probe: dot(en, ldir0) stats + a last sample.
double g_sun_ndl_sum = 0; unsigned long g_sun_ndl_n = 0, g_sun_ndl_pos = 0;
float g_sun_ndl_max = -2.f, g_dbg_en[3] = {0}, g_dbg_ld0[3] = {0};

// ── N6.6 GX texgen (per-texcoord UV generation) ─────────────────────────────────
// A TEV stage references a GX texcoord (0..7); each texcoord is GENERATED by GX from
// a source (vertex tex attr / position / normal) times a texgen matrix. We port that
// natively: read the material's J3DTexGenBlockBasic (mTexCoord[8] @ blk+0x8, each 4 B:
// type@+0 src@+1 mtx@+2; mTexMtx*[8] @ blk+0x28), and for each texcoord compute
// uv = TexMtx · input per vertex in the producer. The matrix is the LIVE computed
// J3DTexMtx::mTotalMtx @ ptr+0x64 (the matrix J3DTexMtx::load feeds GXLoadTexMtxImm),
// Mtx 3x4 row-major. Input = (s,t,1,1) for TEX src, (x,y,z,1) for POS/NRM. This works
// for both J3D SRT conventions (translation in matrix col2 OR col3, the other = 0).
struct TexGen {
    u8   type = 1;          // GXTexGenType: 0=MTX3x4, 1=MTX2x4
    u8   src  = 4;          // GXTexGenSrc: 0=POS,1=NRM,4..11=TEX0..7,19=COLOR0
    bool has_mtx = false;   // a non-identity texgen matrix is set
    float m[12] = {0};      // mTotalMtx 3x4 (row-major) when has_mtx
};
struct TexGenSet { u8 num = 0; TexGen tg[8]; };
TexGenSet g_cur_texgen;

// Diagnostic histograms (kept).
unsigned long g_tg_src_hist[24] = {0}, g_tg_type_hist[12] = {0};
unsigned long g_tg_mtx_id = 0, g_tg_mtx_set = 0, g_tg_n = 0;

void capture_texgen(u32 material) {
    g_cur_texgen = TexGenSet{};
    const u32 blk = r32(material + 0x24);   // J3DMaterial::mTexGenBlock
    if (!valid(blk)) return;
    const u8* B = sb_ram_fast(blk);
    if (!B) return;
    u32 num = r32(blk + 0x04); if (num > 8) num = 8;   // mTexGenNum @ blk+0x4
    g_cur_texgen.num = (u8)num;
    for (u32 i = 0; i < num; i++) {
        const u8 type = B[0x08 + i * 4], src = B[0x09 + i * 4], mtx = B[0x0A + i * 4];
        TexGen& g = g_cur_texgen.tg[i];
        g.type = type; g.src = src;
        // Matrix selector: GX_TEXMTX0=30, step 3; GX_IDENTITY=60 → none.
        if (mtx >= 30 && mtx < 60) {
            const u32 idx = (u32)(mtx - 30) / 3;           // → mTexMtx[] slot
            const u32 mp = idx < 8 ? r32(blk + 0x28 + idx * 4) : 0;   // J3DTexMtx*
            if (valid(mp)) { for (int k = 0; k < 12; k++) g.m[k] = rf(mp + 0x64 + k * 4); g.has_mtx = true; }
        }
        // diagnostics
        if (src < 24) g_tg_src_hist[src]++;
        if (type < 12) g_tg_type_hist[type]++;
        if (mtx == 60) g_tg_mtx_id++; else g_tg_mtx_set++;
        g_tg_n++;
    }
}

// Compute the texgen'd UV for one texcoord of one vertex (the GX per-vertex texgen).
inline void texgen_uv(const TexGen& g, const NgxVertex& v, float out[2]) {
    float in4[4];
    if (g.src >= 4 && g.src <= 11) { const int t = g.src - 4; in4[0]=v.tex[t][0]; in4[1]=v.tex[t][1]; in4[2]=1.f; in4[3]=1.f; }
    else if (g.src == 0)           { in4[0]=v.pos[0]; in4[1]=v.pos[1]; in4[2]=v.pos[2]; in4[3]=1.f; }  // POS
    else if (g.src == 1)           { in4[0]=v.nrm[0]; in4[1]=v.nrm[1]; in4[2]=v.nrm[2]; in4[3]=1.f; }  // NRM
    else                           { in4[0]=v.tex[0][0]; in4[1]=v.tex[0][1]; in4[2]=1.f; in4[3]=1.f; } // COLOR0/SRTG → tex0 fallback
    if (!g.has_mtx) { out[0]=in4[0]; out[1]=in4[1]; return; }   // identity passthrough
    const float* m = g.m;
    float s = m[0]*in4[0]+m[1]*in4[1]+m[2]*in4[2]+m[3]*in4[3];
    float t = m[4]*in4[0]+m[5]*in4[1]+m[6]*in4[2]+m[7]*in4[3];
    if (g.type == 0) {                                          // MTX3x4 → divide by q
        const float q = m[8]*in4[0]+m[9]*in4[1]+m[10]*in4[2]+m[11]*in4[3];
        if (q > 1e-6f || q < -1e-6f) { s /= q; t /= q; }
    }
    out[0]=s; out[1]=t;
}

// Read the material's J3DColorBlock into g_cur_chan (best-effort; valid=false on a
// missing/unknown block → the consumer falls back to the raw vertex colour).
void capture_colorchan(u32 material) {
    g_cur_chan = ChanInfo{};
    const u32 cb = r32(material + 0x20);   // J3DMaterial::mColorBlock
    if (!valid(cb)) return;
    const u32 vt = r32(cb + 0x00);
    const u8* B = sb_ram_fast(cb);
    if (!B) return;
    for (int i = 0; i < 8; i++) {           // colour-block vtable histogram
        if (g_cbvt_key[i] == vt) { g_cbvt_cnt[i]++; break; }
        if (g_cbvt_key[i] == 0) { g_cbvt_key[i] = vt; g_cbvt_cnt[i] = 1; break; }
    }
    if (!g_dbg_cb_have) {                    // one-shot raw dump of the first block
        for (int k = 0; k < 0x44; k++) g_dbg_cb_raw[k] = B[k];
        g_dbg_cb_vt = vt; g_dbg_cb_have = true;
    }
    // Channel control + matColor + ambient come from the J3D color block (synchronous guest
    // RAM — the function-level GX hooks miss J3D's direct-XF-write fast path, and xfmem lags
    // at draw time because the GPU thread is async). matColor@0x04 matches xfmem (white).
    u32 chan_off, amb_off = 0, cull_off;
    if (vt == VT_CLON)      { chan_off = 0x16; amb_off = 0x0C; cull_off = 0x40; }  // J3DColorBlockLightOn
    else if (vt == VT_CLOF) { chan_off = 0x0E; cull_off = 0x16; }                  // J3DColorBlockLightOff
    else return;
    g_cur_chan.cullMode = B[cull_off];
    for (int k = 0; k < 4; k++) g_cur_chan.matColor[k] = B[0x04 + k];
    g_cur_chan.color0 = (u16)((B[chan_off + 0] << 8) | B[chan_off + 1]);   // mColorChan[0] (COLOR0)
    g_cur_chan.alpha0 = (u16)((B[chan_off + 2] << 8) | B[chan_off + 3]);   // mColorChan[1] (ALPHA0)
    // Ambient: LightOn blocks store it (+0x0C). LightOff blocks store NO ambient → it is 0
    // (xfmem ground truth = 0; the global GXSetChanAmbColor register we used before was a
    // stale/wrong purple value that washed + tinted lit register-colour surfaces).
    if (amb_off) for (int k = 0; k < 4; k++) g_cur_chan.ambColor[k] = B[amb_off + k];
    else         for (int k = 0; k < 4; k++) g_cur_chan.ambColor[k] = 0;
    g_cur_chan.valid = true;
}

// GX per-vertex colour-channel lighting for COLOR0/ALPHA0 (faithful to the GC
// hardware lighting model; math cross-checked vs Dolphin VertexShaderGen, re-
// derived). eye = eye-space position, en = eye-space UNIT normal, vcol0 = vertex
// CLR0 (0..1). Writes the lit RGBA (0..1) to out. With lighting disabled this
// reduces to the channel's material source (register colour or vertex colour), so
// the common vertex-lit world materials are unchanged.
bool g_nolight = (getenv("SUNBRIGHT_NGX_NOLIGHT") != nullptr);   // A/B diag: bypass lighting
// DBG per-light breakdown for one floor (up-facing, reg-color, lit) vertex.
bool g_dbgL_done = false, g_dbgL_active = false; int g_dbgL_n = 0; u16 g_dbgL_cc = 0;
int g_dbgL_i[8] = {0}; float g_dbgL_attn[8]={0}, g_dbgL_ndl[8]={0}, g_dbgL_dist[8]={0}, g_dbgL_contrib[8]={0};
float g_dbgL_amb[3]={0}, g_dbgL_illum[3]={0};
void light_vertex(const float eye[3], const float en[3], const float vcol0[4], float out[4]) {
    const ChanInfo& C = g_cur_chan;
    if (g_nolight || !C.valid) { for (int k = 0; k < 4; k++) out[k] = vcol0[k]; return; }
    const u16 cc = C.color0, ca = C.alpha0;
    const bool matVtx = (cc >> 0) & 1;      // GXColorSrc: REG=0, VTX=1
    const bool enable = (cc >> 1) & 1;
    float mat[3];
    if (matVtx) { mat[0] = vcol0[0]; mat[1] = vcol0[1]; mat[2] = vcol0[2]; }
    else { mat[0] = C.matColor[0] / 255.f; mat[1] = C.matColor[1] / 255.f; mat[2] = C.matColor[2] / 255.f; }
    // Alpha channel source (no full alpha lighting in this slice).
    out[3] = ((ca >> 0) & 1) ? vcol0[3] : C.matColor[3] / 255.f;

    if (!enable) {
        out[0] = mat[0]; out[1] = mat[1]; out[2] = mat[2]; g_chan_flat++;
        if (matVtx) g_flat_vtx++; else g_flat_reg++;
        g_flat_lum_sum += (out[0] + out[1] + out[2]) / 3.0;
        return;
    }
    g_chan_lit++;

    const bool ambVtx = (cc >> 6) & 1;
    float illum[3];
    if (ambVtx) { illum[0] = vcol0[0]; illum[1] = vcol0[1]; illum[2] = vcol0[2]; }
    else { illum[0] = C.ambColor[0] / 255.f; illum[1] = C.ambColor[1] / 255.f; illum[2] = C.ambColor[2] / 255.f; }

    const int diffFn  = (cc >> 7) & 3;      // GXDiffuseFn: NONE=0 SIGN=1 CLAMP=2
    const int attnSel = (cc >> 9) & 3;      // 0/2 → NONE, 1 → SPEC, 3 → SPOT (J3DColorChan::getAttnFn)
    const u8  mask    = (u8)(((cc >> 2) & 0x0F) | (((cc >> 11) & 0x0F) << 4));
    // DBG latch: dump the per-light breakdown of the FIRST up-facing reg-color lit vertex.
    if (!g_dbgL_done && !matVtx && !ambVtx && en[1] > 0.85f) {
        g_dbgL_active = true; g_dbgL_n = 0; g_dbgL_done = true; g_dbgL_cc = cc;
        g_dbgL_amb[0]=illum[0]; g_dbgL_amb[1]=illum[1]; g_dbgL_amb[2]=illum[2];
    }
    for (int i = 0; i < 8; i++) {
        if (!(mask & (1 << i)) || !g_light[i].valid) continue;
        const LightObj& L = g_light[i];
        float ld[3] = { L.pos[0] - eye[0], L.pos[1] - eye[1], L.pos[2] - eye[2] };
        const float dist2 = ld[0]*ld[0] + ld[1]*ld[1] + ld[2]*ld[2];
        const float dist  = std::sqrt(dist2);
        if (dist > 1e-6f) { ld[0] /= dist; ld[1] /= dist; ld[2] /= dist; }
        float attn = 1.f;
        if (attnSel == 1) {                 // GX_AF_SPEC
            const float ndl = en[0]*ld[0] + en[1]*ld[1] + en[2]*ld[2];
            const float ang = ndl >= 0.f ? std::fmax(0.f, en[0]*L.dir[0] + en[1]*L.dir[1] + en[2]*L.dir[2]) : 0.f;
            const float a = std::fmax(0.f, L.cosA[0] + L.cosA[1]*ang + L.cosA[2]*ang*ang);
            const float k = L.distA[0] + L.distA[1]*ang + L.distA[2]*ang*ang;
            attn = (k > 1e-6f) ? a / k : 0.f;
        } else if (attnSel == 3) {          // GX_AF_SPOT
            const float c = std::fmax(0.f, ld[0]*L.dir[0] + ld[1]*L.dir[1] + ld[2]*L.dir[2]);
            const float a = std::fmax(0.f, L.cosA[0] + L.cosA[1]*c + L.cosA[2]*c*c);
            const float k = L.distA[0] + L.distA[1]*dist + L.distA[2]*dist2;
            attn = (k > 1e-6f) ? a / k : 0.f;
        }
        float diff = 1.f;
        const float ndl = en[0]*ld[0] + en[1]*ld[1] + en[2]*ld[2];
        if (diffFn == 1) diff = ndl;                    // SIGN
        else if (diffFn == 2) diff = std::fmax(0.f, ndl);  // CLAMP
        const float s = attn * diff;
        illum[0] += s*L.color[0]; illum[1] += s*L.color[1]; illum[2] += s*L.color[2];
        g_diff_sum += diff; g_diff_n++;
        // DBG: capture per-light breakdown for ONE representative up-facing (floor)
        // vertex of a register-color lit material (matsrc=reg, ambsrc=reg) — wash suspect.
        if (g_dbgL_active && g_dbgL_n < 8) {
            g_dbgL_i[g_dbgL_n]=i; g_dbgL_attn[g_dbgL_n]=attn; g_dbgL_ndl[g_dbgL_n]=ndl;
            g_dbgL_dist[g_dbgL_n]=dist; g_dbgL_contrib[g_dbgL_n]=s*L.color[0]; g_dbgL_n++;
        }
        if (i == 0) {   // sun alignment probe
            g_sun_ndl_sum += ndl; g_sun_ndl_n++;
            if (ndl > 0.f) g_sun_ndl_pos++;
            if (ndl > g_sun_ndl_max) {
                g_sun_ndl_max = ndl;
                for (int k = 0; k < 3; k++) { g_dbg_en[k] = en[k]; g_dbg_ld0[k] = ld[k]; }
            }
        }
    }
    if (g_dbgL_active) { g_dbgL_active = false;
        g_dbgL_illum[0]=illum[0]; g_dbgL_illum[1]=illum[1]; g_dbgL_illum[2]=illum[2]; }
    // DBG: average + max illum for up-facing reg-color lit verts (the visible floor/ground).
    if (!matVtx && en[1] > 0.7f) {
        double il = (illum[0]+illum[1]+illum[2])/3.0;
        g_uplit_sum += il; g_uplit_n++;
        if (il > g_uplit_max) { g_uplit_max = il;
            g_uplit_amb = (C.ambColor[0]+C.ambColor[1]+C.ambColor[2])/3.0/255.0;
            g_uplit_ndl = en[0]*g_dbg_ld0[0]+en[1]*g_dbg_ld0[1]+en[2]*g_dbg_ld0[2]; }
    }
    for (int k = 0; k < 3; k++) {
        float v = illum[k]; v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
        out[k] = mat[k] * v;
    }
    g_lit_lum_sum += (out[0] + out[1] + out[2]) / 3.0;
    g_dbg_cc = cc;
    for (int k = 0; k < 4; k++) { g_dbg_mat[k] = C.matColor[k]; g_dbg_amb[k] = C.ambColor[k]; g_dbg_out[k] = out[k]; }
    for (int k = 0; k < 3; k++) g_dbg_illum[k] = illum[k];
}

// ── N5 per-material TEV capture (docs/native_port_plan.md §6) ───────────────────
// The current material is reachable at J3DShape::draw time via the j3dSys global:
// J3DMatPacket::draw sets j3dSys.mMatPacket (+0x3C) to the packet whose material
// it is about to load, then draws that packet's shape packets (→ J3DShape::draw).
// So mMatPacket+0x38 (J3DMatPacket::unk38) is the live J3DMaterial; +0x28 is its
// J3DTevBlock. The block's concrete variant (TVB1/2/4/16 — different field
// offsets) is identified by its vtable pointer (gmse01 addresses; the relative
// 0x9C spacing/order is from the JP symbol map __vt__*J3DTevBlock*, anchored to
// TVB1 = 0x803E0BE8 disassembled from __dt__12J3DTevBlock1). Verified at runtime
// by the /ngxshape vtable histogram.
constexpr u32 VT_TVB16 = 0x803E0A14u, VT_TVB4 = 0x803E0AB0u,
              VT_TVB2  = 0x803E0B4Cu, VT_TVB1 = 0x803E0BE8u;

// Per-variant field offsets (from reference/sms J3DTevBlocks.hpp). stage_off and
// order_off step by 8 / 4 bytes respectively; sentinel 0 = field absent.
struct TevLayout {
    u32 stagenum_off;   // mTevStageNum (0 ⇒ implicit 1 stage, TVB1)
    u32 order_off;      // mTevOrder[0]
    u32 stage_off;      // mTevStage[0]
    u32 tevcolor_off;   // mTevColor[0]  (0 ⇒ none, TVB1)
    u32 kcolor_off;     // mTevKColor[0] (0 ⇒ none)
    u32 kcsel_off;      // mTevKColorSel[0] (0 ⇒ none)
    u32 kasel_off;      // mTevKAlphaSel[0] (0 ⇒ none)
};
inline bool tev_layout(u32 vt, TevLayout& L) {
    switch (vt) {
    case VT_TVB1:  L = {0,      0x06, 0x0A, 0,     0,     0,     0    }; return true;
    case VT_TVB2:  L = {0x30,   0x08, 0x31, 0x10,  0x41,  0x51,  0x53 }; return true;
    case VT_TVB4:  L = {0x1C,   0x0C, 0x1D, 0x3E,  0x5E,  0x6E,  0x72 }; return true;
    case VT_TVB16: L = {0x54,   0x14, 0x55, 0xD6,  0xF6,  0x106, 0x116}; return true;
    default: return false;
    }
}

// N6.7: read the material's per-texmap texture binding OBJECT-MODEL into g_mat_tex.
// The TEV block holds mTexNo[]@+0x04 (u16 per texmap, count by variant); j3dSys's
// J3DTexture (J3DSYS+0x54: mResourceCount u16@0x0, ResTIMG* mResources@0x4) is the
// model's texture table. ResTIMG (size 0x20): format@0x00, width@0x02, height@0x04,
// colorFormat(TLUT)@0x09, paletteOffset@0x0C, imageDataOffset@0x1C — both offsets
// self-relative to the (relocated) header. Image addr = timg + imageDataOffset.
void capture_textures(u32 tevblock, u32 vt) {
    for (int m = 0; m < 8; m++) g_mat_tex[m] = NgxTexBind{};
    const int ntm = vt == VT_TVB16 ? 8 : vt == VT_TVB4 ? 4 : vt == VT_TVB2 ? 2 : 1;
    const u32 jtex = r32(J3DSYS + 0x54);          // j3dSys.mTexture (J3DTexture*)
    if (!valid(jtex)) return;
    const u32 count = (r32(jtex + 0x00) >> 16) & 0xFFFF;   // mResourceCount (u16 @ 0x00)
    const u32 res = r32(jtex + 0x04);             // ResTIMG* mResources
    if (!valid(res) || count == 0) return;
    const u8* tb = sb_ram_fast(tevblock);
    if (!tb) return;
    for (int m = 0; m < ntm; m++) {
        const u16 texNo = (u16)((tb[0x04 + m * 2] << 8) | tb[0x04 + m * 2 + 1]);
        if (texNo == 0xFFFF || texNo >= count) continue;
        const u32 timg = res + (u32)texNo * 0x20;
        const u8* T = sb_ram_fast(timg);
        if (!T) continue;
        NgxTexBind& d = g_mat_tex[m];
        d.fmt = T[0x00];
        d.w = (u16)((T[0x02] << 8) | T[0x03]);
        d.h = (u16)((T[0x04] << 8) | T[0x05]);
        d.tlut_fmt = T[0x09];                     // colorFormat (TLUT fmt for CI)
        const u32 imgOff = ((u32)T[0x1C] << 24) | ((u32)T[0x1D] << 16) | ((u32)T[0x1E] << 8) | T[0x1F];
        d.addr = timg + imgOff;
        if (d.fmt == 0x8 || d.fmt == 0x9 || d.fmt == 0xA) {   // CI → palette
            const u32 palOff = ((u32)T[0x0C] << 24) | ((u32)T[0x0D] << 16) | ((u32)T[0x0E] << 8) | T[0x0F];
            d.tlut_addr = timg + palOff;
        }
    }
}

// ── N7 PE block (alpha test + blend + zmode) ────────────────────────────────────
// The material's mPEBlock @ +0x30 decides framebuffer behaviour. Four variants,
// identified by the block's vtable getType() return (self-identifying — no region-
// specific vtable addresses hardcoded): 'PEOP' opaque, 'PEED' TexEdge cutout
// (alpha-tested foliage), 'PEXL' xlu (alpha blend) are PRESETS storing no fields;
// 'PEFL' full stores J3DAlphaComp/J3DBlend/J3DZMode. The preset GX state is ported
// verbatim from J3DPEBlock*::load (reference/sms J3DMaterial.cpp). The J3DAlphaComp
// / J3DZMode IDs decode as a plain bitfield — that IS what makeAlphaCmpTable /
// makeZModeTable build (J3DTevs.cpp): alphaID=(comp0<<5)|(op<<3)|comp1,
// zID=(cmpEn<<4)|(func<<1)|updEn.
constexpr u32 PE_OP = 0x50454F50u /*'PEOP'*/, PE_ED = 0x50454544u /*'PEED'*/,
              PE_XL = 0x5045584Cu /*'PEXL'*/, PE_FL = 0x5045464Cu /*'PEFL'*/;

// Decode a J3D PE block's getType() tag from its vtable. Stored vtable ptr → slot 0
// at +8 (this codebase's CW/GC convention, see sms_drawsync_lossproof.cpp); getType
// is virtual slot 2 → vtable+0x10. Its body builds the 32-bit FourCC as
// `lis r3,HI; {ori|addi} r3,r3,LO; blr` (the compiler picks ori or addi). Returns
// 0 if it can't be decoded.
u32 pe_block_type(u32 vptr) {
    if (!valid(vptr)) return 0;
    const u32 fn = r32(vptr + 0x10);          // virtual slot 2 = getType
    if (!valid(fn)) return 0;
    const u32 i0 = r32(fn), i1 = r32(fn + 4);
    if ((i0 >> 26) != 15 || ((i0 >> 21) & 31) != 3) return 0;   // addis r3,0,HI (lis r3)
    const u32 hi = i0 & 0xFFFF;
    int lo = 0;
    const u32 op1 = i1 >> 26;
    if (op1 == 24)                                 lo = (int)(i1 & 0xFFFF);  // ori (zero-ext)
    else if (op1 == 14 && ((i1 >> 16) & 31) == 3)  lo = (int16_t)(i1 & 0xFFFF); // addi (sign-ext)
    return (u32)((int)(hi << 16) + lo);
}

// Diagnostics: distinct PE vtable → decoded tag + per-tag use count.
struct PeVtEntry { u32 vt = 0, tag = 0, fn = 0, i0 = 0, i1 = 0; unsigned cnt = 0; };
PeVtEntry g_pe_vt[8];
unsigned long g_pe_op = 0, g_pe_ed = 0, g_pe_xl = 0, g_pe_fl = 0, g_pe_unk = 0, g_pe_none = 0;
unsigned long g_pe_alpha = 0, g_pe_blend = 0, g_pe_nozwrite = 0;

inline bool alpha_always_pass(int comp0, int aop, int comp1) {
    const bool t0 = comp0 == 7, f0 = comp0 == 0;   // ALWAYS / NEVER
    const bool t1 = comp1 == 7, f1 = comp1 == 0;
    switch (aop) {
    case 0:  return t0 && t1;              // AND
    case 1:  return t0 || t1;              // OR
    case 2:  return (t0 && f1) || (f0 && t1);   // XOR
    default: return (t0 && t1) || (f0 && f1);   // XNOR
    }
}

// Read the material's PE block into st.pe. Default (no/unknown block) = opaque:
// depth test+write LEQUAL, no blend, no alpha test.
void capture_pe(u32 material, NgxTevState& st) {
    st.pe = NgxPEState{};
    st.pe.z_test = 1; st.pe.z_func = 3 /*GX_LEQUAL*/; st.pe.z_write = 1;
    const u32 peb = r32(material + 0x30);              // J3DMaterial::mPEBlock
    if (!valid(peb)) { g_pe_none++; return; }
    const u32 vt = r32(peb + 0x00);
    const u32 tag = pe_block_type(vt);

    for (int i = 0; i < 8; i++) {                       // vtable→tag histogram
        if (g_pe_vt[i].vt == vt) { g_pe_vt[i].cnt++; break; }
        if (g_pe_vt[i].vt == 0) {
            g_pe_vt[i].vt = vt; g_pe_vt[i].tag = tag; g_pe_vt[i].cnt = 1;
            g_pe_vt[i].fn = r32(vt + 0x10);
            g_pe_vt[i].i0 = r32(g_pe_vt[i].fn); g_pe_vt[i].i1 = r32(g_pe_vt[i].fn + 4);
            break;
        }
    }

    if (tag == PE_OP) {            // opaque: ALWAYS alpha, no blend, z LEQUAL test+write
        g_pe_op++;                 // defaults already opaque
    } else if (tag == PE_ED) {     // TexEdge cutout (foliage): alpha >=0x80 AND <=0xff
        g_pe_ed++;
        st.pe.alpha_test = 1;
        st.pe.comp0 = 6 /*GEQUAL*/; st.pe.ref0 = 0x80; st.pe.aop = 0 /*AND*/;
        st.pe.comp1 = 3 /*LEQUAL*/; st.pe.ref1 = 0xFF;
        g_pe_alpha++;
    } else if (tag == PE_XL) {     // xlu: no alpha, SRCALPHA/INVSRCALPHA blend, z test, NO write
        g_pe_xl++;
        st.pe.blend_mode = 1 /*GX_BM_BLEND*/;
        st.pe.src_factor = 4 /*GX_BL_SRCALPHA*/; st.pe.dst_factor = 5 /*GX_BL_INVSRCALPHA*/;
        st.pe.logic_op = 3 /*GX_LO_COPY*/;
        st.pe.z_test = 1; st.pe.z_func = 3; st.pe.z_write = 0;
        g_pe_blend++; g_pe_nozwrite++;
    } else if (tag == PE_FL) {     // full — read the stored J3DAlphaComp/J3DBlend/J3DZMode
        g_pe_fl++;
        const u8* B = sb_ram_fast(peb);
        if (B) {
            const u16 acid = (u16)((B[0x08] << 8) | B[0x09]);    // mAlphaComp.mAlphaCmpID
            const u8 ref0 = B[0x0A], ref1 = B[0x0B];
            if (acid != 0xFFFF) {
                const int comp0 = (acid >> 5) & 7, aop = (acid >> 3) & 3, comp1 = acid & 7;
                if (!alpha_always_pass(comp0, aop, comp1)) {
                    st.pe.alpha_test = 1;
                    st.pe.comp0 = (u8)comp0; st.pe.ref0 = ref0; st.pe.aop = (u8)aop;
                    st.pe.comp1 = (u8)comp1; st.pe.ref1 = ref1;
                    g_pe_alpha++;
                }
            }
            st.pe.blend_mode = B[0x0C]; st.pe.src_factor = B[0x0D];   // J3DBlendInfo
            st.pe.dst_factor = B[0x0E]; st.pe.logic_op = B[0x0F];
            if (st.pe.blend_mode == 1 || st.pe.blend_mode == 3) g_pe_blend++;
            const u16 zid = (u16)((B[0x10] << 8) | B[0x11]);          // mZMode.mZModeID
            if (zid != 0xFFFF) {
                st.pe.z_test = (zid >> 4) & 1; st.pe.z_func = (zid >> 1) & 7; st.pe.z_write = zid & 1;
                if (!st.pe.z_write) g_pe_nozwrite++;
            }
        }
    } else {
        g_pe_unk++;                // unknown / undecodable block — keep opaque default
    }
}

// The TEV-state table: deduped by key, persistent across the frame (materials are
// bounded ~ hundreds). Batches reference a state by index. Reset only if it grows
// past the cap (defensive; not expected to be hit).
constexpr size_t TEVSTATE_CAP = 4096;
std::vector<NgxTevState>          g_tevstates;
std::unordered_map<uint64_t, int> g_tevkey_index;
u16 g_tev_cc[TEVSTATE_CAP] = {0};   // DBG: last colour-channel ctrl per tev index (matVtx/enable)
int      g_cur_tev_index = -1;     // material index for the shape being captured
unsigned long g_mat_found = 0, g_mat_novt = 0, g_mat_none = 0;
u32      g_vt_hist_key[8] = {0};    // distinct vtable values seen
unsigned g_vt_hist_cnt[8] = {0};
unsigned g_stage_hist[17] = {0};    // num-stages histogram (0..16)

const unsigned char* resolve(unsigned addr, void*) { return sb_ram_fast(addr); }

// Build the CP descriptor for a shape from its J3D objects. Returns false if the
// descriptor/format objects are missing or malformed.
bool build_cp(u32 sh, NgxCP& cp) {
    const u32 desc  = r32(sh + 0x2C);          // GXVtxDescList*
    const u32 vdata = r32(sh + 0x44);          // J3DVertexData*
    if (!valid(desc) || !valid(vdata)) return false;
    const u32 fmt = r32(vdata + 0x0C);         // GXVtxAttrFmtList*
    if (!valid(fmt)) return false;
    const bool nbt = rb(sh + 0x30) & 1;

    // VCD from the descriptor list.
    for (u32 e = desc; e < desc + 0x200; e += 8) {
        const u32 attr = r32(e), type = r32(e + 4);
        if (attr == 0xFF) break;
        if (attr == 0)               cp.vcd_lo |= (type ? 1u : 0u) << 0;       // PNMTXIDX
        else if (attr <= 8)          cp.vcd_lo |= (type ? 1u : 0u) << attr;    // TEXiMTXIDX
        else if (attr == 9)          cp.vcd_lo |= (type & 3) << 9;             // POS
        else if (attr == 10)         cp.vcd_lo |= (type & 3) << 11;            // NRM
        else if (attr == 11)         cp.vcd_lo |= (type & 3) << 13;            // CLR0
        else if (attr == 12)         cp.vcd_lo |= (type & 3) << 15;            // CLR1
        else if (attr >= 13 && attr <= 20) cp.vcd_hi |= (type & 3) << (2 * (attr - 13));  // TEX
    }

    // VAT (+ array strides via attribute formats) from the format list.
    u32 pos_type = 4, nrm_type = 4, tex_type[8] = {4,4,4,4,4,4,4,4};
    u32& A = cp.vat[0][0]; u32& B = cp.vat[0][1]; u32& C = cp.vat[0][2];
    for (u32 e = fmt; e < fmt + 0x400; e += 16) {
        const u32 attr = r32(e), cnt = r32(e + 4), type = r32(e + 8), frac = rb(e + 12) & 0x1f;
        if (attr == 0xFF) break;
        switch (attr) {
        case 9:  A |= (cnt & 1) | ((type & 7) << 1) | (frac << 4); pos_type = type; break;
        case 10: A |= ((cnt ? 1u : 0u) << 9) | ((type & 7) << 10); if (cnt == 2) A |= 1u << 31;
                 nrm_type = type; break;
        case 11: A |= ((cnt & 1) << 13) | ((type & 7) << 14); break;
        case 12: A |= ((cnt & 1) << 17) | ((type & 7) << 18); break;
        case 13: A |= ((cnt & 1) << 21) | ((type & 7) << 22) | (frac << 25); tex_type[0] = type; break;
        case 14: B |= (cnt & 1) | ((type & 7) << 1) | (frac << 4);            tex_type[1] = type; break;
        case 15: B |= ((cnt & 1) << 9)  | ((type & 7) << 10) | (frac << 13);  tex_type[2] = type; break;
        case 16: B |= ((cnt & 1) << 18) | ((type & 7) << 19) | (frac << 22);  tex_type[3] = type; break;
        case 17: B |= ((cnt & 1) << 27) | ((type & 7) << 28); C |= frac;      tex_type[4] = type; break;
        case 18: C |= ((cnt & 1) << 5)  | ((type & 7) << 6)  | (frac << 9);   tex_type[5] = type; break;
        case 19: C |= ((cnt & 1) << 14) | ((type & 7) << 15) | (frac << 18);  tex_type[6] = type; break;
        case 20: C |= ((cnt & 1) << 23) | ((type & 7) << 24) | (frac << 27);  tex_type[7] = type; break;
        default: break;
        }
    }

    // Live vertex-array bases + strides. loadVtxArray() overrides pos/nrm/clr0 with j3dSys's
    // per-view buffers (unk10C/110/114), BUT only when those are set: for STATIC map geometry
    // the per-view CLR0 buffer (unk114) is NULL and GX keeps the static BMD colour array baked
    // by makeVtxArrayCmd (J3DVertexData::mVtxColorArray[0] @ vdata+0x1C; CLR1 = [1] @ +0x20).
    // Using unk114 unconditionally made indexed CLR0 resolve to null → white → washed-out floor/
    // buildings/sky (vertex-coloured map geometry). Fall back to the static array when null.
    cp.array_base[0] = r32(J3DSYS + 0x10C);  cp.array_stride[0] = (pos_type == 4) ? 12 : 6;
    cp.array_base[1] = nbt ? r32(vdata + 0x18) : r32(J3DSYS + 0x110);
    cp.array_stride[1] = ((nrm_type == 4) ? 12 : 6) * (nbt ? 3 : 1);
    // CLR0 array: take it from Dolphin's live CP state (the array GXSetArray actually bound
    // for this shape — g_main_cp_state is updated by the recompiled draw that just ran). The
    // engine picks per-view LIT colours (j3dSys.unk114) vs the static authored base each frame;
    // reading unk114 ourselves got the WRONG one (stale null → static bright base → washed-out).
    // Dolphin stores the address with the 0x8000_0000 region bit masked off → OR it back.
    const u32 cp_clr0 = g_main_cp_state.array_bases[CPArray::Color0];
    cp.array_base[2] = cp_clr0 ? (cp_clr0 | 0x80000000u) : r32(vdata + 0x1C);
    cp.array_stride[2] = g_main_cp_state.array_strides[CPArray::Color0] ?: 4;
    cp.array_base[3] = r32(vdata + 0x20);    cp.array_stride[3] = 4;
    for (int i = 0; i < 8; i++) {
        cp.array_base[4 + i]   = r32(vdata + 0x24 + i * 4);
        cp.array_stride[4 + i] = (tex_type[i] == 4) ? 8 : 4;
    }
    return true;
}

// ── Published stats (read best-effort by /ngxshape from the HTTP thread; the
//    emu/render thread is the only writer, so torn reads at worst misreport a
//    counter by one — acceptable for a diagnostic) ─────────────────────────────
unsigned long g_calls = 0, g_meshes = 0, g_fail = 0, g_badcp = 0;
unsigned long g_total_verts = 0, g_total_tris = 0;
unsigned      g_last_verts = 0, g_last_tris = 0, g_max_verts = 0;
u32           g_last_vcdlo = 0, g_last_vcdhi = 0, g_last_vstride = 0;
float         g_last_pos[3] = {0, 0, 0};
// Native XF (vertex-transform) verification: model-space positions transformed by
// the game's modelview (j3dSys.mCurrentDrawMtx) → eye space. On-screen geometry
// faces the camera (GC camera looks down −Z), so eye.z<0 is "in front".
unsigned long g_xf_total = 0, g_xf_front = 0, g_xf_nomtx = 0;
float         g_last_eye[3] = {0, 0, 0};
float         g_eye_min[3] = {0, 0, 0}, g_eye_max[3] = {0, 0, 0};
// Native projection stage: the latest perspective projection matrix (4x4 row-
// major, as the game passes to GXSetProjection), published by the scene_render
// GXSetProjection tee. eye → clip = P·(eye,1) → NDC = clip.xyz/clip.w. On-screen
// geometry has clip.w>0 (in front of the near plane) and NDC x,y in [-1,1].
float         g_proj[16] = {0};
bool          g_have_proj = false;
unsigned long g_ndc_total = 0, g_ndc_wpos = 0, g_ndc_inbox = 0;

// Clip-space triangle SNAPSHOT for the native Vulkan mesh render (vk_mesh.cpp /
// ngx_present.cpp): one frame's scene triangles + per-texture draw batches.
//
// DOUBLE-BUFFERED per frame. The game thread accumulates the current frame into
// the `g_cur` buffer; at each frame boundary (GXSetProjection, ngx_frame_begin)
// the just-completed buffer is published to `g_front` (atomic) and accumulation
// flips to the other buffer. The present (video thread) always reads the last
// COMPLETE frame from `g_front` — never a half-accumulated one. Before this, a
// single live buffer was read mid-accumulation/mid-wrap, so a present that landed
// just after the buffer wrapped showed a partial scene (the plaza floor missing,
// or near-empty) → intermittent black frames.
constexpr size_t SNAP_CAP  = 600000;   // vertices (200k tris) per buffer
constexpr size_t BATCH_CAP = 8192;     // draw batches per buffer
std::vector<NgxRenderVertex> g_snap[2];   // SNAP_CAP entries each, lazily sized
size_t                       g_snap_count[2] = {0, 0};
std::vector<NgxRenderBatch>  g_batches[2];
int                          g_cur = 0;       // accumulation buffer (game thread only)
std::atomic<int>             g_front{0};       // published buffer (present reads)
int                          g_read_front = 0; // latched by ngx_snap_verts for batches
unsigned long                g_frame_swaps = 0;

// Reusable scratch (single emu/render thread serialized by nthr).
std::vector<NgxVertex> g_verts;
std::vector<unsigned>  g_indices;
std::vector<float>     g_clip;        // 4 floats/vertex (clip-space), scratch
std::vector<float>     g_litrgba;     // 4 floats/vertex (lit raster colour0), scratch
std::vector<float>     g_uvs;         // 16 floats/vertex (8 texgen'd UVs), scratch

// Read the current material's TEV block into an NgxTevState, dedupe it into the
// table, and return its index (-1 if no material / unknown block variant). Reads
// the guest object straight from RAM (big-endian bytes via sb_ram_fast), object-
// model — no GX byte-stream decode.
int capture_material() {
    g_cur_chan = ChanInfo{};                           // reset; stays invalid if no material
    g_cur_texgen = TexGenSet{};                        // reset texgen (num=0 → raw-attr fallback)
    for (int m = 0; m < 8; m++) g_mat_tex[m] = NgxTexBind{};   // reset textures
    const u32 matpacket = r32(J3DSYS + 0x3C);          // j3dSys.mMatPacket
    if (!valid(matpacket)) { g_mat_none++; return -1; }
    const u32 material = r32(matpacket + 0x38);        // J3DMatPacket::unk38
    if (!valid(material)) { g_mat_none++; return -1; }
    capture_colorchan(material);                       // N6: colour-channel/lighting state
    capture_texgen(material);                          // diag: texgen src/type scope
    const u32 tevblock = r32(material + 0x28);         // J3DMaterial::mTevBlock
    if (!valid(tevblock)) { g_mat_none++; return -1; }
    const u32 vt = r32(tevblock + 0x00);               // J3DTevBlock vtable ptr

    // Vtable histogram (verification) — record up to 8 distinct values.
    for (int i = 0; i < 8; i++) {
        if (g_vt_hist_key[i] == vt) { g_vt_hist_cnt[i]++; break; }
        if (g_vt_hist_key[i] == 0) { g_vt_hist_key[i] = vt; g_vt_hist_cnt[i] = 1; break; }
    }

    capture_textures(tevblock, vt);                    // N6.7: object-model per-texmap textures

    TevLayout L;
    if (!tev_layout(vt, L)) { g_mat_novt++; return -1; }
    const u8* B = sb_ram_fast(tevblock);               // raw big-endian bytes
    if (!B) { g_mat_none++; return -1; }
    auto rb8  = [&](u32 o) -> u8  { return B[o]; };
    auto rs16 = [&](u32 o) -> int16_t { return (int16_t)((B[o] << 8) | B[o + 1]); };

    NgxTevState st{};
    u8 ns = L.stagenum_off ? rb8(L.stagenum_off) : 1;
    if (ns < 1) ns = 1; if (ns > 16) ns = 16;
    st.num_stages = ns;
    for (int s = 0; s < ns; s++) {
        const u32 so = L.stage_off + s * 8;            // J3DTevStage (8 bytes)
        // color_env = [mTevColorOp:mTevColorAB:mTevColorCD] (low 24 bits of the BP word)
        st.stage[s].color_env = ((u32)rb8(so + 1) << 16) | ((u32)rb8(so + 2) << 8) | rb8(so + 3);
        // alpha_env = [mTevAlphaOp:mTevAlphaAB:mTevSwapModeInfo]
        st.stage[s].alpha_env = ((u32)rb8(so + 5) << 16) | ((u32)rb8(so + 6) << 8) | rb8(so + 7);
        const u32 oo = L.order_off + s * 4;            // J3DTevOrder (4 bytes)
        st.stage[s].texcoord   = rb8(oo + 0);
        st.stage[s].texmap     = rb8(oo + 1);
        st.stage[s].color_chan = rb8(oo + 2);
        st.stage[s].kcsel = L.kcsel_off ? rb8(L.kcsel_off + s) : 0x0C;  // GX_TEV_KCSEL_1 default
        st.stage[s].kasel = L.kasel_off ? rb8(L.kasel_off + s) : 0x1C;  // GX_TEV_KASEL_1 default
    }
    // 4 TEV color registers (CPREV/C0/C1/C2), S10 RGBA — absent on TVB1 (defaults 0).
    if (L.tevcolor_off)
        for (int c = 0; c < 4; c++)
            for (int k = 0; k < 4; k++) st.tev_color[c][k] = rs16(L.tevcolor_off + c * 8 + k * 2);
    // 4 KONST color registers, u8 RGBA — absent on TVB1 (defaults 255 = white).
    if (L.kcolor_off) {
        for (int c = 0; c < 4; c++)
            for (int k = 0; k < 4; k++) st.kcolor[c][k] = rb8(L.kcolor_off + c * 4 + k);
    } else {
        std::memset(st.kcolor, 0xFF, sizeof st.kcolor);
    }

    capture_pe(material, st);   // N7: PE block (alpha test → shader, blend/zmode → pipeline)
    st.pe.cull = g_cur_chan.cullMode;   // backface culling (color block) → pipeline cull state

    // FNV-1a key over the captured state (excluding the key field itself).
    uint64_t h = 1469598103934665603ull;
    const u8* p = (const u8*)&st;
    for (size_t i = 0; i < offsetof(NgxTevState, key); i++) { h ^= p[i]; h *= 1099511628211ull; }
    st.key = h;

    if (ns <= 16) g_stage_hist[ns]++;
    g_mat_found++;

    const u16 dbgcc = g_cur_chan.valid ? g_cur_chan.color0 : 0xFFFF;  // 0xFFFF = no colour block
    auto it = g_tevkey_index.find(h);
    if (it != g_tevkey_index.end()) { if (it->second < (int)TEVSTATE_CAP) g_tev_cc[it->second] = dbgcc; return it->second; }
    if (g_tevstates.size() >= TEVSTATE_CAP) { g_tevstates.clear(); g_tevkey_index.clear(); }
    const int idx = (int)g_tevstates.size();
    g_tevstates.push_back(st);
    g_tevkey_index[h] = idx;
    if (idx < (int)TEVSTATE_CAP) g_tev_cc[idx] = dbgcc;
    return idx;
}

// Transform this shape's extracted model-space positions by the live modelview
// matrix (Mtx 3x4 at *j3dSys.mCurrentDrawMtx) and fold into the eye-space stats.
// This is the native XF stage; the matrix is the same one the recompiled J3D
// computed (the interp60 pos-matrix seam, now consumed natively).
void transform_eye() {
    const u32 mp = r32(J3DSYS + 0x104);     // mCurrentDrawMtx (Mtx*)
    if (!valid(mp)) { g_xf_nomtx++; return; }
    float m[12];
    for (int i = 0; i < 12; i++) m[i] = rf(mp + i * 4);   // row-major 3x4
    // Normal matrix (Mtx33, row-major 3x3) for the native lighting stage. Absent →
    // skip lighting (en stays 0 → only ambient contributes, matched by the math).
    const u32 nmp = r32(J3DSYS + 0x108);    // mCurrentNormMtx (Mtx33*)
    const bool have_nm = valid(nmp);
    float nm[9]; if (have_nm) for (int i = 0; i < 9; i++) nm[i] = rf(nmp + i * 4);
    bool first = (g_xf_total == 0);
    const size_t nv = g_verts.size();
    if (g_have_proj) g_clip.assign(nv * 4, 0.0f);
    g_litrgba.assign(nv * 4, 0.0f);
    g_uvs.assign(nv * 16, 0.0f);
    for (size_t vi = 0; vi < nv; vi++) {
        const NgxVertex& v = g_verts[vi];

        // GX texgen: compute each texcoord's UV (texgen'd by the per-material matrix).
        // Texcoords without a texgen def fall back to the raw vertex tex attribute.
        float* uvp = &g_uvs[vi * 16];
        for (int m = 0; m < 8; m++) {
            if (m < g_cur_texgen.num) texgen_uv(g_cur_texgen.tg[m], v, &uvp[m * 2]);
            else { uvp[m * 2 + 0] = v.tex[m][0]; uvp[m * 2 + 1] = v.tex[m][1]; }
        }
        const float x = v.pos[0], y = v.pos[1], z = v.pos[2];
        const float ex = m[0]*x + m[1]*y + m[2]*z  + m[3];
        const float ey = m[4]*x + m[5]*y + m[6]*z  + m[7];
        const float ez = m[8]*x + m[9]*y + m[10]*z + m[11];

        // Native colour-channel lighting → per-vertex raster colour0.
        float en[3] = {0, 0, 0};
        if (have_nm) {
            en[0] = nm[0]*v.nrm[0] + nm[1]*v.nrm[1] + nm[2]*v.nrm[2];
            en[1] = nm[3]*v.nrm[0] + nm[4]*v.nrm[1] + nm[5]*v.nrm[2];
            en[2] = nm[6]*v.nrm[0] + nm[7]*v.nrm[1] + nm[8]*v.nrm[2];
            const float l = std::sqrt(en[0]*en[0] + en[1]*en[1] + en[2]*en[2]);
            if (l > 1e-6f) { en[0]/=l; en[1]/=l; en[2]/=l; }
            g_nrm_len_sum += l; g_nrm_n++;
            if (g_cur_chan.valid && ((g_cur_chan.color0 >> 1) & 1)) {
                g_lit_nrm_sum += l; g_lit_nrm_n++;
                if (l < 0.1f) g_lit_nrm_zero++;
            }
        }
        const float eye[3] = { ex, ey, ez };
        const float vcol0[4] = { v.clr[0][0]/255.f, v.clr[0][1]/255.f,
                                 v.clr[0][2]/255.f, v.clr[0][3]/255.f };
        light_vertex(eye, en, vcol0, &g_litrgba[vi * 4]);
        // DBG: bucket the resulting col0 luminance by category to localize the bright bulk.
        { const float* o = &g_litrgba[vi*4]; double lum=(o[0]+o[1]+o[2])/3.0;
          const bool valid=g_cur_chan.valid; const bool mv=valid&&((g_cur_chan.color0>>0)&1);
          const bool en2=valid&&((g_cur_chan.color0>>1)&1);
          int cat = !valid ? 4 : (mv?2:0)+(en2?1:0);   // 0 reg/flat,1 reg/lit,2 vtx/flat,3 vtx/lit,4 noblock
          g_colcat_sum[cat]+=lum; g_colcat_n[cat]++; }

        g_xf_total++;
        if (ez < 0.0f) g_xf_front++;
        if (first) { g_eye_min[0]=g_eye_max[0]=ex; g_eye_min[1]=g_eye_max[1]=ey;
                     g_eye_min[2]=g_eye_max[2]=ez; first=false; }
        else { if(ex<g_eye_min[0])g_eye_min[0]=ex; if(ex>g_eye_max[0])g_eye_max[0]=ex;
               if(ey<g_eye_min[1])g_eye_min[1]=ey; if(ey>g_eye_max[1])g_eye_max[1]=ey;
               if(ez<g_eye_min[2])g_eye_min[2]=ez; if(ez>g_eye_max[2])g_eye_max[2]=ez; }
        g_last_eye[0]=ex; g_last_eye[1]=ey; g_last_eye[2]=ez;

        // Native projection: clip = P·(eye,1); NDC = clip.xyz / clip.w.
        if (g_have_proj) {
            const float* p = g_proj;
            const float cx = p[0]*ex + p[1]*ey + p[2]*ez + p[3];
            const float cy = p[4]*ex + p[5]*ey + p[6]*ez + p[7];
            const float cz = p[8]*ex + p[9]*ey + p[10]*ez + p[11];
            const float cw = p[12]*ex + p[13]*ey + p[14]*ez + p[15];
            float* cp = &g_clip[vi * 4]; cp[0]=cx; cp[1]=cy; cp[2]=cz; cp[3]=cw;
            g_ndc_total++;
            if (cw > 0.0f) {
                g_ndc_wpos++;
                const float nx = cx / cw, ny = cy / cw;
                if (nx >= -1.0f && nx <= 1.0f && ny >= -1.0f && ny <= 1.0f) g_ndc_inbox++;
            }
        }
    }

    // Accumulate this shape's clip-space triangles into the render snapshot,
    // grouped into a per-texture batch (triangle-aligned roll-over so a wrap
    // never splits a triangle; on wrap the batch list resets with the buffer).
    if (g_have_proj && !g_indices.empty()) {
        std::vector<NgxRenderVertex>& snap = g_snap[g_cur];
        std::vector<NgxRenderBatch>&  batches = g_batches[g_cur];
        size_t& count = g_snap_count[g_cur];
        if (snap.size() < SNAP_CAP) snap.resize(SNAP_CAP);
        // Per-texmap bindings for this shape's material (object-model, g_mat_tex).
        // CI formats (C4/C8/C14X2) need a resolved TLUT; else render that texmap flat.
        NgxTexBind tb[8] = {};
        for (int m = 0; m < 8; m++) {
            const NgxTexBind& c = g_mat_tex[m];
            const bool is_ci = (c.fmt == 0x8 || c.fmt == 0x9 || c.fmt == 0xA);
            if (c.addr && c.w && c.h && (!is_ci || c.tlut_addr)) tb[m] = c;
        }
        for (size_t t = 0; t + 3 <= g_indices.size(); t += 3) {
            if (count + 3 > SNAP_CAP) { count = 0; batches.clear(); }  // safety wrap (huge frame)
            // Open a new batch on material/binding change, after a wrap, or at start.
            const bool tex_diff = batches.empty() ||
                memcmp(batches.back().tex, tb, sizeof tb) != 0;
            if (batches.empty() || tex_diff ||
                batches.back().tev_index != g_cur_tev_index ||
                batches.back().vstart + batches.back().vcount != (uint32_t)count) {
                if (batches.size() >= BATCH_CAP) break;  // bounded
                NgxRenderBatch nb{}; memcpy(nb.tex, tb, sizeof tb);
                nb.vstart = (uint32_t)count; nb.vcount = 0; nb.tev_index = g_cur_tev_index;
                batches.push_back(nb);
            }
            for (int e = 0; e < 3; e++) {
                const unsigned vidx = g_indices[t + e];
                const float* cp = &g_clip[vidx * 4];
                const float* lit = &g_litrgba[vidx * 4];
                const float* uvp = &g_uvs[vidx * 16];
                NgxRenderVertex& d = snap[count];
                d.clip[0]=cp[0]; d.clip[1]=cp[1]; d.clip[2]=cp[2]; d.clip[3]=cp[3];
                d.rgba[0]=lit[0]; d.rgba[1]=lit[1]; d.rgba[2]=lit[2]; d.rgba[3]=lit[3];
                for (int m = 0; m < 8; m++) { d.uv[m][0]=uvp[m*2+0]; d.uv[m][1]=uvp[m*2+1]; }
                count++;
            }
            batches.back().vcount += 3;
        }
    }
}

void capture(u32 sh) {
    g_calls++;
    NgxCP cp{};
    if (!build_cp(sh, cp)) { g_badcp++; return; }

    const u32 nelem = r32(sh + 4) & 0xFFFF;        // mElementCount @0x6
    const u32 draws = r32(sh + 0x38);              // J3DShapeDraw**
    if (!nelem || !valid(draws)) return;

    g_verts.clear();
    g_indices.clear();
    int tris = 0;
    bool any_fail = false;
    for (u32 i = 0; i < nelem && i < 64; i++) {
        const u32 dp = r32(draws + i * 4);
        if (!valid(dp)) continue;
        const u32 size = r32(dp + 4);              // mDisplayListSize (vtable@0)
        const u32 list = r32(dp + 8);              // mDisplayList
        const unsigned char* host = sb_ram_fast(list);
        if (!host || size == 0 || size > 0x200000) continue;
        const int t = ngx_build_mesh(cp, host, size, resolve, nullptr, g_verts, g_indices);
        if (t < 0) any_fail = true; else tris += t;
    }

    g_meshes++;
    if (any_fail) g_fail++;
    g_total_verts += g_verts.size();
    g_total_tris  += (unsigned)tris;
    g_last_verts = (unsigned)g_verts.size();
    g_last_tris  = (unsigned)tris;
    g_last_vcdlo = cp.vcd_lo; g_last_vcdhi = cp.vcd_hi;
    g_last_vstride = ngx_vertex_size(cp, 0);
    if (g_last_verts > g_max_verts) g_max_verts = g_last_verts;
    // DBG: CLR0-class + matsrc histograms (vert-weighted) to localize the wash.
    { const unsigned cls = (cp.vcd_lo>>13)&3; g_clr0cls_hist[cls] += g_verts.size();
      const unsigned cfmt = (cp.vat[0][0]>>14)&7; if (cfmt<8) g_clr0fmt_hist[cfmt] += g_verts.size();
      const bool mv = g_cur_chan.valid && ((g_cur_chan.color0>>0)&1);
      const bool en = g_cur_chan.valid && ((g_cur_chan.color0>>1)&1);
      g_matsrc_hist[g_cur_chan.valid ? (mv?1:0) : 2] += g_verts.size();
      g_litcfg_hist[en?1:0] += g_verts.size();
      // capture the biggest no-normal (map) shape's CLR0 first-vertex color + class
      const bool has_nrm = ((cp.vcd_lo>>11)&3)!=0;
      if (!has_nrm && g_verts.size() > g_bigmap_verts) {
          g_bigmap_verts = g_verts.size(); g_bigmap_clr0cls = cls;
          g_bigmap_matvtx = mv; g_bigmap_en = en;
          g_bigmap_vcol[0]=g_verts[0].clr[0][0]; g_bigmap_vcol[1]=g_verts[0].clr[0][1];
          g_bigmap_vcol[2]=g_verts[0].clr[0][2]; g_bigmap_cc=g_cur_chan.color0;
      }
      // biggest shape OVERALL (likely the visible floor/building): cc + normals + vcol0
      if (g_verts.size() > g_bigany_verts) {
          g_bigany_verts = g_verts.size(); g_bigany_cc = g_cur_chan.color0;
          g_bigany_hasnrm = has_nrm; g_bigany_matvtx = mv; g_bigany_en = en;
          g_bigany_clr0cls = cls;
          g_bigany_vcol[0]=g_verts[0].clr[0][0]; g_bigany_vcol[1]=g_verts[0].clr[0][1]; g_bigany_vcol[2]=g_verts[0].clr[0][2];
          double s=0; for (auto&vv:g_verts) s+=(vv.clr[0][0]+vv.clr[0][1]+vv.clr[0][2])/3.0;
          g_bigany_vcolmean = s/g_verts.size();
          g_bigany_clr0base = cp.array_base[2];
      }
    }
    if (!g_verts.empty()) {
        g_last_pos[0] = g_verts[0].pos[0];
        g_last_pos[1] = g_verts[0].pos[1];
        g_last_pos[2] = g_verts[0].pos[2];
    }
    g_cur_tev_index = capture_material();   // N5: current J3DMaterial's TEV state
    transform_eye();   // native XF stage (modelview) + eye-space verification
}

}  // namespace

// Published by the scene_render GXSetProjection tee (0x80362c34) with the
// authored projection matrix. Only perspective (type 0 = GX_PERSPECTIVE) is kept
// — the J3D world uses it; 2D HUD uses orthographic which we don't transform here.
void ngx_set_projection(const float* m44, unsigned type) {
    if (type != 0) return;
    for (int i = 0; i < 16; i++) g_proj[i] = m44[i];
    g_have_proj = true;
}

// Explicit per-frame boundary, called from the J2DScreen::draw tee (scene_render.cpp):
// the HUD draws ONCE per frame AFTER all 3D drawing, so the accumulation buffer holds
// a complete 3D frame at that point. Publish it to the front buffer and flip
// accumulation to the other one — the present (video thread) then always reads a whole
// frame, never a half-accumulated one. The empty-guard makes repeat HUD draws within a
// frame (dialogue etc.) no-ops, and it aligns the 3D publish with the J2D HUD snapshot
// (both taken at the same tee → consistent composited frame). NOT GXSetProjection
// (fires ~5×/frame); NOT a GX HW function (GXCopyDisp can't be safely super-called).
void ngx_frame_publish() {
    if (g_snap_count[g_cur] == 0 && g_batches[g_cur].empty()) return;  // no 3D this frame: keep last
    g_front.store(g_cur, std::memory_order_release);
    g_cur ^= 1;
    if (g_snap[g_cur].size() < SNAP_CAP) g_snap[g_cur].resize(SNAP_CAP);
    g_snap_count[g_cur] = 0;
    g_batches[g_cur].clear();
    g_frame_swaps++;
}

// Best-effort snapshot accessors for the native Vulkan mesh render (copy promptly
// — the emu thread keeps writing; a torn read at worst yields a stray triangle).
const NgxRenderVertex* ngx_snap_verts(int* nverts) {
    // Latch the published buffer index so ngx_snap_batches reads the SAME frame
    // (the caller fetches verts then batches; a swap between them would mismatch).
    g_read_front = g_front.load(std::memory_order_acquire);
    *nverts = (int)g_snap_count[g_read_front];
    return g_snap[g_read_front].empty() ? nullptr : g_snap[g_read_front].data();
}
const NgxRenderBatch* ngx_snap_batches(int* nbatches) {
    const int f = g_read_front;
    *nbatches = (int)g_batches[f].size();
    return g_batches[f].empty() ? nullptr : g_batches[f].data();
}
const NgxTevState* ngx_snap_tevstates(int* nstates) {
    *nstates = (int)g_tevstates.size();
    return g_tevstates.empty() ? nullptr : g_tevstates.data();
}
// DBG: colour-channel ctrl for a tev index (0xFFFF = no block) — used by the present's
// category-debug mode to tint each batch by its material category.
extern "C" unsigned ngx_tev_cc_dbg(int idx) {
    return (idx >= 0 && idx < (int)TEVSTATE_CAP) ? g_tev_cc[idx] : 0;
}

// GXLoadTexObj(GXTexObj* obj, GXTexMapID id) @ 0x80360160 — track the texmap-0
// binding so shapes drawn after it can be textured. GXTexObj packed fields
// (reference/sms GXInitTexObj + docs/re_notes/efb_native_60fps.md): image0@0x08 =
// width-1[0:9] / height-1[10:19] / fmt&0xF[20:23]; image3@0x0C = (addr>>5)[0:20].
// GXLoadTlut(GXTlutObj* tlut_obj, u32 tlut_name) @ 0x803601fc — record the palette
// for a TMEM tlut slot. __GXTlutObjInt: tlut@0x00 (fmt@bits10-11), loadTlut0@0x04
// (lut addr>>5 @bits0-20). CI texobjs reference the slot via texobj.tlutName@0x18.
SUNBRIGHT_OVERRIDE(ov_gxloadtlut, 0x803601fcu) {
    if (g_enabled) {
        const u32 obj = cpu.gpr[3], name = cpu.gpr[4] & 0xFF;
        if (valid(obj)) {
            const u32 tlut = r32(obj + 0x00), loadTlut0 = r32(obj + 0x04);
            g_tlut[name].fmt = (u8)((tlut >> 10) & 3);
            g_tlut[name].addr = 0x80000000u | ((loadTlut0 & 0x1FFFFFu) << 5);
            g_tlut[name].valid = true;
        }
    }
    if (RecompFunc o = recomp_raw(0x803601fcu)) o(cpu); else call_ppc(cpu, cpu.lr);
}

// Per-texmap GXLoadTexObj + Preloaded histograms (diagnostic).
unsigned long g_texobj_hist[9] = {0}, g_preload_hist[9] = {0};
// Distinct texmap-0 texture addresses ever bound (bounded set) + last-batch stats.
u32 g_distinct_addr[512]; unsigned g_distinct_n = 0; bool g_distinct_overflow = false;
void note_addr(u32 a) {
    if (a == 0) return;
    for (unsigned i = 0; i < g_distinct_n; i++) if (g_distinct_addr[i] == a) return;
    if (g_distinct_n < 512) g_distinct_addr[g_distinct_n++] = a; else g_distinct_overflow = true;
}

// GXLoadTexObjPreLoaded(GXTexObj* obj, GXTexRegion* region, GXTexMapID id) @ 0x8035ffb8.
SUNBRIGHT_OVERRIDE(ov_gxloadtexobjpreloaded, 0x8035ffb8u) {
    if (g_enabled) {
        const u32 id = cpu.gpr[5];
        g_preload_hist[id < 8 ? id : 8]++;
        if (id < 8 && valid(cpu.gpr[3])) decode_texobj(cpu.gpr[3], g_curtex[id]);
    }
    if (RecompFunc o = recomp_raw(0x8035ffb8u)) o(cpu); else call_ppc(cpu, cpu.lr);
}

// GXLoadTexObj(GXTexObj* obj, GXTexMapID id) @ 0x80360160 — capture the binding for
// EVERY texmap (0..7) so per-stage TEV texmap selection samples the right texture.
SUNBRIGHT_OVERRIDE(ov_gxloadtexobj, 0x80360160u) {
    if (g_enabled) {
        const u32 id = cpu.gpr[4];
        g_texobj_hist[id < 8 ? id : 8]++;
        if (id < 8 && valid(cpu.gpr[3])) { decode_texobj(cpu.gpr[3], g_curtex[id]); note_addr(g_curtex[id].addr); }
    }
    if (RecompFunc o = recomp_raw(0x80360160u)) o(cpu); else call_ppc(cpu, cpu.lr);
}

// GXLoadLightObjImm(GXLightObj* lit, GXLightID id) @ 0x8035f26c — capture the live
// hardware light state for the native lighting stage. id is a GXLightID bit mask
// (GX_LIGHT0=0x01 .. GX_LIGHT7=0x80); a light may be loaded into several slots.
SUNBRIGHT_OVERRIDE(ov_gxloadlightobjimm, 0x8035f26cu) {
    if (g_enabled) {
        const u32 obj = cpu.gpr[3], id = cpu.gpr[4];
        if (valid(obj)) {
            LightObj L; L.valid = true;
            const u32 col = r32(obj + 0x0C);
            L.color[0] = ((col >> 24) & 0xFF) / 255.f;
            L.color[1] = ((col >> 16) & 0xFF) / 255.f;
            L.color[2] = ((col >>  8) & 0xFF) / 255.f;
            L.cosA[0]  = rf(obj + 0x10); L.cosA[1]  = rf(obj + 0x14); L.cosA[2]  = rf(obj + 0x18);
            L.distA[0] = rf(obj + 0x1C); L.distA[1] = rf(obj + 0x20); L.distA[2] = rf(obj + 0x24);
            L.pos[0]   = rf(obj + 0x28); L.pos[1]   = rf(obj + 0x2C); L.pos[2]   = rf(obj + 0x30);
            L.dir[0]   = rf(obj + 0x34); L.dir[1]   = rf(obj + 0x38); L.dir[2]   = rf(obj + 0x3C);
            for (int i = 0; i < 8; i++) if (id & (1u << i)) g_light[i] = L;
            g_light_loads++;
        }
    }
    if (RecompFunc o = recomp_raw(0x8035f26cu)) o(cpu); else call_ppc(cpu, cpu.lr);
}

// GXSetChanCtrl(chan, enable, amb_src, mat_src, light_mask, diff_fn, attn_fn) @ 0x8035f6d0 —
// capture the AUTHORITATIVE per-channel lighting control, reconstructed exactly as GX packs
// it into the XF channel register (reference dolphin/gx GXSetChanCtrl). This is the same
// LitChannel.hex Dolphin's renderer uses — captured SYNCHRONOUSLY on the emu thread (the J3D
// color-block byte parse was reading the wrong offsets → wrong enable/ambient/diffuse).
SUNBRIGHT_OVERRIDE(ov_gxsetchanctrl, 0x8035f6d0u) {
    if (g_enabled) {
        const u32 chan = cpu.gpr[3], enable = cpu.gpr[4], amb_src = cpu.gpr[5],
                  mat_src = cpu.gpr[6], lmask = cpu.gpr[7], diff_fn = cpu.gpr[8], attn_fn = cpu.gpr[9];
        u32 reg = 0;
        reg |= (enable & 1) << 1;
        reg |= (mat_src & 1) << 0;
        reg |= (amb_src & 1) << 6;
        if (lmask & 0x01) reg |= 1u << 2;  if (lmask & 0x02) reg |= 1u << 3;
        if (lmask & 0x04) reg |= 1u << 4;  if (lmask & 0x08) reg |= 1u << 5;
        if (lmask & 0x10) reg |= 1u << 11; if (lmask & 0x20) reg |= 1u << 12;
        if (lmask & 0x40) reg |= 1u << 13; if (lmask & 0x80) reg |= 1u << 14;
        reg |= ((attn_fn == 0) ? 0u : (diff_fn & 3)) << 7;   // diffuse (zeroed when SPEC)
        reg |= ((attn_fn != 2) ? 1u : 0u) << 9;              // attn bit 9
        reg |= ((attn_fn != 0) ? 1u : 0u) << 10;             // attn bit 10
        const int idx = (chan == 4) ? 0 : (chan == 5) ? 1 : (chan <= 1 ? (int)chan : -1);
        if (idx >= 0) { g_gx_cc[idx] = (u16)reg; g_gx_cc_have[idx] = true; }
    }
    if (RecompFunc o = recomp_raw(0x8035f6d0u)) o(cpu); else call_ppc(cpu, cpu.lr);
}

// GXSetChanMatColor(GXChannelID chan, GXColor color) @ 0x8035f51c — material colour register.
SUNBRIGHT_OVERRIDE(ov_gxsetchanmatcolor, 0x8035f51cu) {
    if (g_enabled) {
        const u32 chan = cpu.gpr[3], c = cpu.gpr[4];
        const int idx = (chan == 0 || chan == 4) ? 0 : (chan == 1 || chan == 5) ? 1 : -1;
        if (idx >= 0) {
            g_gx_matcol[idx][0] = (u8)(c >> 24); g_gx_matcol[idx][1] = (u8)(c >> 16);
            g_gx_matcol[idx][2] = (u8)(c >> 8);  g_gx_matcol[idx][3] = (u8)c;
            g_gx_matcol_have[idx] = true;
        }
    }
    if (RecompFunc o = recomp_raw(0x8035f51cu)) o(cpu); else call_ppc(cpu, cpu.lr);
}

// GXSetChanAmbColor(GXChannelID chan, GXColor color) @ 0x8035f3b4 — capture the
// global hardware ambient register. GXChannelID: COLOR0=0, COLOR1=1, COLOR0A0=4,
// COLOR1A1=5. color is a 4-byte GXColor passed by value in gpr[4] (R in high byte).
SUNBRIGHT_OVERRIDE(ov_gxsetchanambcolor, 0x8035f3b4u) {
    if (g_enabled) {
        const u32 chan = cpu.gpr[3], c = cpu.gpr[4];
        const int idx = (chan == 0 || chan == 4) ? 0 : (chan == 1 || chan == 5) ? 1 : -1;
        if (idx >= 0) {
            g_amb_reg[idx][0] = (u8)(c >> 24); g_amb_reg[idx][1] = (u8)(c >> 16);
            g_amb_reg[idx][2] = (u8)(c >> 8);  g_amb_reg[idx][3] = (u8)c;
            g_amb_have[idx] = true; g_amb_sets++;
        }
    }
    if (RecompFunc o = recomp_raw(0x8035f3b4u)) o(cpu); else call_ppc(cpu, cpu.lr);
}

SUNBRIGHT_OVERRIDE(ov_j3dshape_draw, 0x802e0390u) {
    const u32 sh = cpu.gpr[3];   // save before the super-call clobbers gpr
    // Run the real draw FIRST: J3DShape::draw is what sets j3dSys's per-view vertex
    // arrays (loadVtxArray) AND the modelview (setModelDrawMtx) for THIS shape, so
    // we must capture after it for the arrays + matrix to be current.
    if (RecompFunc o = recomp_raw(0x802e0390u)) o(cpu); else call_ppc(cpu, cpu.lr);
    if (g_enabled) capture(sh);
}

// Probe report (/ngxshape).
int sb_ngx_shape_dump(char* out, int cap) {
    int n = snprintf(out, cap,
        "ngx J3DShape capture: %s\n"
        "  calls=%lu  meshes_built=%lu  badcp=%lu  framing_fail=%lu\n"
        "  cumulative: verts=%lu tris=%lu\n"
        "  last shape: verts=%u tris=%u vstride=%u vcd_lo=%08x vcd_hi=%08x\n"
        "  last pos[0]=(%.3f, %.3f, %.3f)  max_verts/shape=%u\n"
        "  native XF (modelview): xf_verts=%lu  in_front(z<0)=%lu (%.1f%%)  no_mtx=%lu\n"
        "  eye bbox: x[%.1f..%.1f] y[%.1f..%.1f] z[%.1f..%.1f]  last_eye=(%.2f, %.2f, %.2f)\n"
        "  native projection: have_proj=%d  clip.w>0=%lu/%lu  NDC xy in [-1,1]=%lu (%.1f%%)  frame_swaps=%lu\n",
        g_enabled ? "ON" : "OFF (set SUNBRIGHT_NGX_SHAPE=1)",
        g_calls, g_meshes, g_badcp, g_fail,
        g_total_verts, g_total_tris,
        g_last_verts, g_last_tris, g_last_vstride, g_last_vcdlo, g_last_vcdhi,
        g_last_pos[0], g_last_pos[1], g_last_pos[2], g_max_verts,
        g_xf_total, g_xf_front, g_xf_total ? 100.0 * (double)g_xf_front / (double)g_xf_total : 0.0,
        g_xf_nomtx,
        g_eye_min[0], g_eye_max[0], g_eye_min[1], g_eye_max[1], g_eye_min[2], g_eye_max[2],
        g_last_eye[0], g_last_eye[1], g_last_eye[2],
        g_have_proj ? 1 : 0, g_ndc_wpos, g_ndc_total,
        g_ndc_inbox, g_ndc_total ? 100.0 * (double)g_ndc_inbox / (double)g_ndc_total : 0.0,
        g_frame_swaps);

    // N5 per-material TEV capture stats.
    n += snprintf(out + n, cap - n,
        "  TEV material: found=%lu none=%lu unknown_vt=%lu  unique_states=%zu\n"
        "  TevBlock vtable histogram:\n",
        g_mat_found, g_mat_none, g_mat_novt, g_tevstates.size());
    for (int i = 0; i < 8 && g_vt_hist_key[i]; i++) {
        const u32 vt = g_vt_hist_key[i];
        const char* nm = vt == VT_TVB1 ? "TVB1" : vt == VT_TVB2 ? "TVB2" :
                         vt == VT_TVB4 ? "TVB4" : vt == VT_TVB16 ? "TV16" : "????";
        n += snprintf(out + n, cap - n, "    %08x %-4s  %u\n", vt, nm, g_vt_hist_cnt[i]);
    }
    // N6 lighting capture stats.
    n += snprintf(out + n, cap - n,
        "  lighting: light_loads=%lu  lit_verts=%lu (mean_lum=%.3f, mean_diff=%.3f)\n"
        "    flat_verts=%lu (reg=%lu vtx=%lu, mean_lum=%.3f)\n"
        "    last lit: cc=%04x mat=(%u,%u,%u,%u) amb=(%u,%u,%u,%u) illum=(%.2f,%.2f,%.2f) out=(%.2f,%.2f,%.2f,%.2f)\n",
        g_light_loads, g_chan_lit,
        g_chan_lit ? g_lit_lum_sum / g_chan_lit : 0.0,
        g_diff_n ? g_diff_sum / g_diff_n : 0.0,
        g_chan_flat, g_flat_reg, g_flat_vtx,
        g_chan_flat ? g_flat_lum_sum / g_chan_flat : 0.0,
        g_dbg_cc, g_dbg_mat[0], g_dbg_mat[1], g_dbg_mat[2], g_dbg_mat[3],
        g_dbg_amb[0], g_dbg_amb[1], g_dbg_amb[2], g_dbg_amb[3],
        g_dbg_illum[0], g_dbg_illum[1], g_dbg_illum[2],
        g_dbg_out[0], g_dbg_out[1], g_dbg_out[2], g_dbg_out[3]);
    for (int i = 0; i < 8; i++) if (g_light[i].valid) {
        const LightObj& L = g_light[i];
        n += snprintf(out + n, cap - n,
            "    light[%d] col=(%.2f,%.2f,%.2f) pos=(%.0f,%.0f,%.0f) dir=(%.2f,%.2f,%.2f)\n"
            "             cosA=(%.3f,%.3f,%.3f) distA=(%.3g,%.3g,%.3g)\n",
            i, L.color[0], L.color[1], L.color[2], L.pos[0], L.pos[1], L.pos[2],
            L.dir[0], L.dir[1], L.dir[2],
            L.cosA[0], L.cosA[1], L.cosA[2], L.distA[0], L.distA[1], L.distA[2]);
    }
    n += snprintf(out + n, cap - n,
        "    mean |normal|=%.3f (n=%lu)  lit-vert |normal|=%.3f  lit-zero-normal=%lu/%lu (%.1f%%)\n",
        g_nrm_n ? g_nrm_len_sum / g_nrm_n : 0.0, g_nrm_n,
        g_lit_nrm_n ? g_lit_nrm_sum / g_lit_nrm_n : 0.0,
        g_lit_nrm_zero, g_lit_nrm_n, g_lit_nrm_n ? 100.0 * g_lit_nrm_zero / g_lit_nrm_n : 0.0);
    n += snprintf(out + n, cap - n,
        "    sun dot(en,ldir0): mean=%.3f max=%.3f pos_frac=%.1f%% (n=%lu)\n"
        "      @max: en=(%.2f,%.2f,%.2f) ld0=(%.2f,%.2f,%.2f)\n",
        g_sun_ndl_n ? g_sun_ndl_sum / g_sun_ndl_n : 0.0, g_sun_ndl_max,
        g_sun_ndl_n ? 100.0 * g_sun_ndl_pos / g_sun_ndl_n : 0.0, g_sun_ndl_n,
        g_dbg_en[0], g_dbg_en[1], g_dbg_en[2], g_dbg_ld0[0], g_dbg_ld0[1], g_dbg_ld0[2]);
    n += snprintf(out + n, cap - n,
        "    amb_reg[0]=(%u,%u,%u,%u) have=%d sets=%lu\n",
        g_amb_reg[0][0], g_amb_reg[0][1], g_amb_reg[0][2], g_amb_reg[0][3],
        g_amb_have[0], g_amb_sets);
    n += snprintf(out + n, cap - n, "    colour-block vtables:");
    for (int i = 0; i < 8 && g_cbvt_key[i]; i++)
        n += snprintf(out + n, cap - n, " %08x(%u)", g_cbvt_key[i], g_cbvt_cnt[i]);
    n += snprintf(out + n, cap - n, "\n");
    if (g_dbg_cb_have) {
        n += snprintf(out + n, cap - n, "    block vt=%08x raw[0x00..0x23]:", g_dbg_cb_vt);
        for (int k = 0; k < 0x24; k++) n += snprintf(out + n, cap - n, "%s%02x",
            (k % 4 == 0) ? " " : "", g_dbg_cb_raw[k]);
        n += snprintf(out + n, cap - n, "\n");
    }
    {   // batch texture stats (published frame): texmap0 presence + distinct addrs
        const std::vector<NgxRenderBatch>& fb = g_batches[g_front.load(std::memory_order_acquire)];
        unsigned tb_tex0 = 0, tb_none = 0, distinct = 0; u32 seen[64];
        for (const auto& B : fb) {
            if (B.tex[0].addr) {
                tb_tex0++;
                bool f = false; for (unsigned i = 0; i < distinct; i++) if (seen[i] == B.tex[0].addr) { f = true; break; }
                if (!f && distinct < 64) seen[distinct++] = B.tex[0].addr;
            }
            bool any = false; for (int m = 0; m < 8; m++) if (B.tex[m].addr) any = true;
            if (!any) tb_none++;
        }
        n += snprintf(out + n, cap - n,
            "    batches=%zu  with_tex0=%u  untextured=%u  distinct_tex0_addr=%u\n"
            "    distinct addrs bound to texmap0 (lifetime)=%u%s\n",
            fb.size(), tb_tex0, tb_none, distinct,
            g_distinct_n, g_distinct_overflow ? "+ (overflow)" : "");
    }
    n += snprintf(out + n, cap - n, "    GXLoadTexObj by texmap:");
    for (int i = 0; i < 9; i++) if (g_texobj_hist[i]) n += snprintf(out + n, cap - n, " [%d]=%lu", i, g_texobj_hist[i]);
    n += snprintf(out + n, cap - n, "   Preloaded:");
    for (int i = 0; i < 9; i++) if (g_preload_hist[i]) n += snprintf(out + n, cap - n, " [%d]=%lu", i, g_preload_hist[i]);
    n += snprintf(out + n, cap - n, "\n");
    n += snprintf(out + n, cap - n, "    texgen src hist:");
    for (int i = 0; i < 24; i++) if (g_tg_src_hist[i]) {
        const char* nm = i==0?"POS":i==1?"NRM":(i>=4&&i<=11)?"TEX":"?";
        n += snprintf(out + n, cap - n, " %s%d=%lu", nm, i, g_tg_src_hist[i]);
    }
    n += snprintf(out + n, cap - n, "  type:");
    for (int i = 0; i < 12; i++) if (g_tg_type_hist[i]) n += snprintf(out + n, cap - n, " t%d=%lu", i, g_tg_type_hist[i]);
    n += snprintf(out + n, cap - n, "  mtx: id=%lu set=%lu (n=%lu)\n", g_tg_mtx_id, g_tg_mtx_set, g_tg_n);
    // N7 PE block (alpha test + blend + zmode) capture stats + vtable→tag table.
    n += snprintf(out + n, cap - n,
        "  PE block: OP=%lu ED=%lu XL=%lu FL=%lu unknown=%lu none=%lu\n"
        "    alpha_tested=%lu  blended=%lu  no-zwrite=%lu\n",
        g_pe_op, g_pe_ed, g_pe_xl, g_pe_fl, g_pe_unk, g_pe_none,
        g_pe_alpha, g_pe_blend, g_pe_nozwrite);
    for (int i = 0; i < 8 && g_pe_vt[i].vt; i++) {
        const u32 t = g_pe_vt[i].tag;
        const char c[5] = { (char)(t >> 24), (char)(t >> 16), (char)(t >> 8), (char)t, 0 };
        n += snprintf(out + n, cap - n,
            "    vt=%08x tag=%08x '%s' getType=%08x [%08x %08x] cnt=%u\n",
            g_pe_vt[i].vt, t, (t >> 24) ? c : "????", g_pe_vt[i].fn,
            g_pe_vt[i].i0, g_pe_vt[i].i1, g_pe_vt[i].cnt);
    }
    n += snprintf(out + n, cap - n, "  num-stages histogram:");
    for (int s = 1; s <= 16; s++) if (g_stage_hist[s])
        n += snprintf(out + n, cap - n, " [%d]=%u", s, g_stage_hist[s]);
    n += snprintf(out + n, cap - n, "\n");
    // Dump the first few captured states' stage-0 combiner registers (sanity).
    int nst = 0; const NgxTevState* sts = ngx_snap_tevstates(&nst);
    for (int i = 0; i < nst && i < 4; i++) {
        n += snprintf(out + n, cap - n,
            "  state[%d]: stages=%u  s0 color_env=%06x alpha_env=%06x map=%u coord=%u chan=%u kc=%02x ka=%02x\n",
            i, sts[i].num_stages, sts[i].stage[0].color_env, sts[i].stage[0].alpha_env,
            sts[i].stage[0].texmap, sts[i].stage[0].texcoord, sts[i].stage[0].color_chan,
            sts[i].stage[0].kcsel, sts[i].stage[0].kasel);
    }
    // FULL dump of states (all stages + konst/tevreg) — combiner audit. Prefer 1-stage.
    int dumped = 0;
    for (int i = 0; i < nst && dumped < 10; i++) {
        const NgxTevState& s = sts[i];
        if (s.num_stages != 1) continue;   // simple materials only (the 46300-draw majority)
        n += snprintf(out + n, cap - n, "  FULL state[%d] stages=%u pe.alpha=%d blend=%d/%d/%d:\n",
                      i, s.num_stages, s.pe.alpha_test, s.pe.blend_mode, s.pe.src_factor, s.pe.dst_factor);
        for (int st = 0; st < s.num_stages; st++)
            n += snprintf(out + n, cap - n,
                "    s%d ce=%06x ae=%06x map=%u coord=%u chan=%u kc=%02x ka=%02x\n",
                st, s.stage[st].color_env, s.stage[st].alpha_env, s.stage[st].texmap,
                s.stage[st].texcoord, s.stage[st].color_chan, s.stage[st].kcsel, s.stage[st].kasel);
        for (int c = 0; c < 4; c++)
            n += snprintf(out + n, cap - n, "    kcolor[%d]=(%u,%u,%u,%u) tevreg[%d]=(%d,%d,%d,%d)\n",
                c, s.kcolor[c][0], s.kcolor[c][1], s.kcolor[c][2], s.kcolor[c][3],
                c, s.tev_color[c][0], s.tev_color[c][1], s.tev_color[c][2], s.tev_color[c][3]);
        dumped++;
    }
    // Ground-truth XF lighting state (what Dolphin's renderer actually uses) for A/B vs our capture.
    {
        auto cu=[&](u32 v){return v;};
        n += snprintf(out+n, cap-n, "  XFMEM lighting (Dolphin ground truth):\n");
        for (int c=0;c<2;c++)
            n += snprintf(out+n, cap-n, "    color[%d].hex=%08x amb=%08x mat=%08x | OURS cc=%04x have=%d amb=(%u,%u,%u) mat=(%u,%u,%u)\n",
                c, xfmem.color[c].hex, cu(xfmem.ambColor[c]), cu(xfmem.matColor[c]),
                g_gx_cc[c], g_gx_cc_have[c], g_amb_reg[c][0],g_amb_reg[c][1],g_amb_reg[c][2],
                g_gx_matcol[c][0],g_gx_matcol[c][1],g_gx_matcol[c][2]);
        for (int i=0;i<4;i++){ const Light&L=xfmem.lights[i];
            n += snprintf(out+n, cap-n, "    L%d col=(%u,%u,%u,%u) cosatt=(%.2f,%.2f,%.2f) distatt=(%.4f,%.4f,%.4f) pos=(%.0f,%.0f,%.0f)\n",
                i, L.color[0],L.color[1],L.color[2],L.color[3], L.cosatt[0],L.cosatt[1],L.cosatt[2],
                L.distatt[0],L.distatt[1],L.distatt[2], L.dpos[0],L.dpos[1],L.dpos[2]); }
    }
    n += snprintf(out+n, cap-n,
        "  CLR0 class hist (verts): notpresent=%lu direct=%lu idx8=%lu idx16=%lu | matsrc: reg=%lu vtx=%lu novalid=%lu | lighting: off=%lu on=%lu\n",
        g_clr0cls_hist[0], g_clr0cls_hist[1], g_clr0cls_hist[2], g_clr0cls_hist[3],
        g_matsrc_hist[0], g_matsrc_hist[1], g_matsrc_hist[2], g_litcfg_hist[0], g_litcfg_hist[1]);
    n += snprintf(out+n, cap-n,
        "  BIGGEST no-normal (map) shape: verts=%zu clr0cls=%u matVtx=%d enable=%d cc=%04x vcol0=(%u,%u,%u)\n",
        g_bigmap_verts, g_bigmap_clr0cls, g_bigmap_matvtx, g_bigmap_en, g_bigmap_cc,
        g_bigmap_vcol[0], g_bigmap_vcol[1], g_bigmap_vcol[2]);
    n += snprintf(out+n, cap-n,
        "  BIGGEST shape overall: verts=%zu cc=%04x (matVtx=%d en=%d) hasNrm=%d clr0cls=%u vcol0=(%u,%u,%u) vcolMean=%.1f\n",
        g_bigany_verts, g_bigany_cc, g_bigany_matvtx, g_bigany_en, g_bigany_hasnrm, g_bigany_clr0cls,
        g_bigany_vcol[0],g_bigany_vcol[1],g_bigany_vcol[2], g_bigany_vcolmean);
    n += snprintf(out+n, cap-n,
        "  CLR0 array base: ours(biggest)=%08x  Dolphin g_main_cp_state[Color0]=%08x stride=%u\n",
        g_bigany_clr0base, g_main_cp_state.array_bases[CPArray::Color0],
        g_main_cp_state.array_strides[CPArray::Color0]);
    n += snprintf(out+n, cap-n,
        "  CLR0 VAT fmt hist (verts): 565=%lu 888=%lu 888x=%lu 4444=%lu 6666=%lu 8888=%lu\n",
        g_clr0fmt_hist[0],g_clr0fmt_hist[1],g_clr0fmt_hist[2],g_clr0fmt_hist[3],g_clr0fmt_hist[4],g_clr0fmt_hist[5]);
    n += snprintf(out+n, cap-n,
        "  col0 lum by category (verts,avg): reg/flat=(%lu,%.2f) reg/lit=(%lu,%.2f) vtx/flat=(%lu,%.2f) vtx/lit=(%lu,%.2f) noblock=(%lu,%.2f)\n",
        g_colcat_n[0], g_colcat_n[0]?g_colcat_sum[0]/g_colcat_n[0]:0.0,
        g_colcat_n[1], g_colcat_n[1]?g_colcat_sum[1]/g_colcat_n[1]:0.0,
        g_colcat_n[2], g_colcat_n[2]?g_colcat_sum[2]/g_colcat_n[2]:0.0,
        g_colcat_n[3], g_colcat_n[3]?g_colcat_sum[3]/g_colcat_n[3]:0.0,
        g_colcat_n[4], g_colcat_n[4]?g_colcat_sum[4]/g_colcat_n[4]:0.0);
    n += snprintf(out+n, cap-n,
        "  up-facing reg-lit illum (visible floor/ground): avg=%.3f max=%.3f (n=%lu) @max amb=%.3f ndl=%.3f\n",
        g_uplit_n?g_uplit_sum/g_uplit_n:0.0, g_uplit_max, g_uplit_n, g_uplit_amb, g_uplit_ndl);
    // CLR0 vertex-color array (J3DSYS+0x114) — indexed map-geometry colours. If this base is
    // wrong/stale, indexed CLR0 lookups fail → white default → washed-out map geometry.
    {
        const u32 ca = r32(J3DSYS + 0x114);
        n += snprintf(out+n, cap-n, "  CLR0 array base=%08x:", ca);
        if (valid(ca)) for (int i = 0; i < 6; i++) n += snprintf(out+n, cap-n, " %08x", r32(ca + i*4));
        n += snprintf(out+n, cap-n, "\n");
    }
    // GX fog state (bpmem) — prime suspect for a global darkening ngx skips.
    n += snprintf(out+n, cap-n, "  FOG (bpmem): fsel=%u proj=%u color(rgb)=(%u,%u,%u) A=%.4f C=%.2f b_mag=%u b_shift=%u\n",
        (u32)bpmem.fog.c_proj_fsel.fsel.Value(), (u32)bpmem.fog.c_proj_fsel.proj.Value(),
        (u32)bpmem.fog.color.r, (u32)bpmem.fog.color.g, (u32)bpmem.fog.color.b,
        bpmem.fog.GetA(), bpmem.fog.GetC(), bpmem.fog.b_magnitude, bpmem.fog.b_shift);
    // DBG floor-vertex lighting breakdown (cc, ambient, per-light contribution, final illum).
    n += snprintf(out+n, cap-n, "  FLOOR-vert lighting: cc=%04x amb=(%.2f,%.2f,%.2f) lights=%d:\n",
        g_dbgL_cc, g_dbgL_amb[0], g_dbgL_amb[1], g_dbgL_amb[2], g_dbgL_n);
    for (int i = 0; i < g_dbgL_n; i++)
        n += snprintf(out+n, cap-n, "    light[%d] attn=%.3f ndl=%+.3f dist=%.0f contrib(r)=%.3f\n",
            g_dbgL_i[i], g_dbgL_attn[i], g_dbgL_ndl[i], g_dbgL_dist[i], g_dbgL_contrib[i]);
    n += snprintf(out+n, cap-n, "    final illum=(%.3f,%.3f,%.3f) → out=illum (mat=white)\n",
        g_dbgL_illum[0], g_dbgL_illum[1], g_dbgL_illum[2]);
    // Top batches by approximate screen area (NDC bbox) → identify the dominant on-screen
    // surfaces (floor/buildings) and the material they use. Helps localize the brightness gap.
    {
        const int fb = g_front.load(std::memory_order_acquire);
        const std::vector<NgxRenderVertex>& snap = g_snap[fb];
        const std::vector<NgxRenderBatch>&  bats = g_batches[fb];
        struct BR { float area; float cy; int ti; uint32_t vc; u32 tex0; };
        std::vector<BR> brs;
        for (const auto& B : bats) {
            float xmn=1e9f,xmx=-1e9f,ymn=1e9f,ymx=-1e9f; int cnt=0; double sy=0;
            for (uint32_t v = B.vstart; v < B.vstart + B.vcount && v < snap.size(); v++) {
                const float* c = snap[v].clip; if (c[3] <= 1e-4f) continue;
                float nx=c[0]/c[3], ny=c[1]/c[3];
                if(nx<xmn)xmn=nx; if(nx>xmx)xmx=nx; if(ny<ymn)ymn=ny; if(ny>ymx)ymx=ny; sy+=ny; cnt++;
            }
            if (cnt < 3) continue;
            float area = (xmx-xmn)*(ymx-ymn);
            brs.push_back({area, (float)(sy/cnt), B.tev_index, B.vcount, B.tex[0].addr});
        }
        std::sort(brs.begin(), brs.end(), [](const BR&a,const BR&b){return a.area>b.area;});
        // Decode the top batches' tex0 raw mean (skip degenerate brs[0]) — the visible floor/
        // buildings — to check whether ngx's DECODED texture is itself too bright vs GX.
        int td=0;
        for (size_t bi = 0; bi < brs.size() && td < 4; bi++) {
            const NgxRenderBatch* bp = nullptr;
            for (const auto& BB : bats) if (BB.tev_index == brs[bi].ti && BB.tex[0].addr) { bp = &BB; break; }
            if (!bp) continue;
            const NgxTexBind& t = bp->tex[0];
            if (!t.w || !t.h || t.w > 1024 || t.h > 1024) continue;
            const unsigned char* src = sb_ram_fast(t.addr);
            const unsigned char* tl = t.tlut_addr ? sb_ram_fast(t.tlut_addr) : nullptr;
            if (!src) continue;
            std::vector<uint32_t> px((size_t)t.w*t.h);
            sb_tex_decode(px.data(), src, t.w, t.h, t.fmt, tl, t.tlut_fmt);
            double r=0,g=0,b=0; for (uint32_t v:px){r+=v&0xFF;g+=(v>>8)&0xFF;b+=(v>>16)&0xFF;}
            size_t nn=px.size();
            n += snprintf(out+n, cap-n, "  TEX0[ti=%d area=%.2f] %08x fmt=%u %ux%u rawmean=(%.0f,%.0f,%.0f)=%.0f\n",
                brs[bi].ti, brs[bi].area, t.addr, t.fmt, t.w, t.h, r/nn, g/nn, b/nn, (r+g+b)/3/nn);
            td++;
        }
        n += snprintf(out+n, cap-n, "  TOP batches by screen area (ndc):\n");
        for (size_t i = 0; i < brs.size() && i < 8; i++) {
            const BR& b = brs[i];
            const NgxTevState* s = (b.ti>=0 && b.ti<nst) ? &sts[b.ti] : nullptr;
            // first vertex's col0 (the rgba fed to the shader) for this batch
            float r=0,g=0,bl=0; for (const auto& B2:bats) if (B2.tev_index==b.ti && B2.vcount){
                if (B2.vstart<snap.size()){ r=snap[B2.vstart].rgba[0]; g=snap[B2.vstart].rgba[1]; bl=snap[B2.vstart].rgba[2]; } break; }
            const u16 bcc = (b.ti>=0 && b.ti<(int)TEVSTATE_CAP) ? g_tev_cc[b.ti] : 0;
            const char* mcat = bcc==0xFFFF ? "noblk" : ((bcc&1)?((bcc&2)?"vtx/lit":"vtx/flat"):((bcc&2)?"reg/lit":"reg/flat"));
            n += snprintf(out+n, cap-n, "    area=%.3f cy=%+.2f ti=%d vc=%u tex0=%08x col0=(%.2f,%.2f,%.2f) cc=%04x[%s]  %s s0ce=%06x chan=%u stg=%u\n",
                b.area, b.cy, b.ti, b.vc, b.tex0, r,g,bl, bcc, mcat,
                s?(s->num_stages==1?"1stage":"multi"):"?",
                s?s->stage[0].color_env:0, s?s->stage[0].color_chan:0, s?s->num_stages:0);
        }
    }
    return n;
}
