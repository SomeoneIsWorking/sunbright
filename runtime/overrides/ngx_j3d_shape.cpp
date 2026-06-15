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
#include <vector>

namespace {

constexpr u32 J3DSYS = 0x804045DCu;
bool g_enabled = (getenv("SUNBRIGHT_NGX_SHAPE") != nullptr);

// Latest GX texmap-0 binding (from the GXLoadTexObj tee), associated with shapes
// drawn after it. Decoded from the GXTexObj's packed registers.
struct CurTex { u32 addr = 0; u16 w = 0, h = 0; u8 fmt = 0; u32 tlut_addr = 0; u8 tlut_fmt = 0; bool valid = false; };
CurTex g_curtex[8];   // live binding per GX texmap (0..7)

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
    u32 chan_off, amb_off = 0;
    if (vt == VT_CLON)      { chan_off = 0x16; amb_off = 0x0C; }
    else if (vt == VT_CLOF) { chan_off = 0x0E; }
    else return;                            // unknown colour-block variant
    for (int k = 0; k < 4; k++) g_cur_chan.matColor[k] = B[0x04 + k];
    // Ambient: LightOn blocks store/load it; LightOff blocks don't, so the ambient
    // is the global hardware register (captured at GXSetChanAmbColor).
    if (amb_off)               for (int k = 0; k < 4; k++) g_cur_chan.ambColor[k] = B[amb_off + k];
    else if (g_amb_have[0])    for (int k = 0; k < 4; k++) g_cur_chan.ambColor[k] = g_amb_reg[0][k];
    g_cur_chan.color0 = (u16)((B[chan_off + 0] << 8) | B[chan_off + 1]);   // mColorChan[0]
    g_cur_chan.alpha0 = (u16)((B[chan_off + 2] << 8) | B[chan_off + 3]);   // mColorChan[1]
    g_cur_chan.valid = true;
}

// GX per-vertex colour-channel lighting for COLOR0/ALPHA0 (faithful to the GC
// hardware lighting model; math cross-checked vs Dolphin VertexShaderGen, re-
// derived). eye = eye-space position, en = eye-space UNIT normal, vcol0 = vertex
// CLR0 (0..1). Writes the lit RGBA (0..1) to out. With lighting disabled this
// reduces to the channel's material source (register colour or vertex colour), so
// the common vertex-lit world materials are unchanged.
void light_vertex(const float eye[3], const float en[3], const float vcol0[4], float out[4]) {
    const ChanInfo& C = g_cur_chan;
    if (!C.valid) { for (int k = 0; k < 4; k++) out[k] = vcol0[k]; return; }
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
        if (i == 0) {   // sun alignment probe
            g_sun_ndl_sum += ndl; g_sun_ndl_n++;
            if (ndl > 0.f) g_sun_ndl_pos++;
            if (ndl > g_sun_ndl_max) {
                g_sun_ndl_max = ndl;
                for (int k = 0; k < 3; k++) { g_dbg_en[k] = en[k]; g_dbg_ld0[k] = ld[k]; }
            }
        }
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

// The TEV-state table: deduped by key, persistent across the frame (materials are
// bounded ~ hundreds). Batches reference a state by index. Reset only if it grows
// past the cap (defensive; not expected to be hit).
constexpr size_t TEVSTATE_CAP = 4096;
std::vector<NgxTevState>          g_tevstates;
std::unordered_map<uint64_t, int> g_tevkey_index;
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

    // Live vertex-array bases + strides (loadVtxArray overrides pos/nrm/clr0 with
    // j3dSys's per-view buffers; clr1/tex come from the static BMD arrays).
    cp.array_base[0] = r32(J3DSYS + 0x10C);  cp.array_stride[0] = (pos_type == 4) ? 12 : 6;
    cp.array_base[1] = nbt ? r32(vdata + 0x18) : r32(J3DSYS + 0x110);
    cp.array_stride[1] = ((nrm_type == 4) ? 12 : 6) * (nbt ? 3 : 1);
    cp.array_base[2] = r32(J3DSYS + 0x114);  cp.array_stride[2] = 4;
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

// Clip-space triangle SNAPSHOT for the native Vulkan mesh render (vk_mesh.cpp):
// a rolling window of recent scene triangles + per-texture draw batches. Read
// best-effort from the HTTP thread (a torn read at worst shows a stray triangle
// — diagnostic-acceptable).
constexpr size_t SNAP_CAP  = 600000;   // vertices (200k tris)
constexpr size_t BATCH_CAP = 8192;     // draw batches
std::vector<NgxRenderVertex> g_snap;   // SNAP_CAP entries, lazily sized
size_t                       g_snap_count = 0;
std::vector<NgxRenderBatch>  g_batches;

// Reusable scratch (single emu/render thread serialized by nthr).
std::vector<NgxVertex> g_verts;
std::vector<unsigned>  g_indices;
std::vector<float>     g_clip;        // 4 floats/vertex (clip-space), scratch
std::vector<float>     g_litrgba;     // 4 floats/vertex (lit raster colour0), scratch

// Read the current material's TEV block into an NgxTevState, dedupe it into the
// table, and return its index (-1 if no material / unknown block variant). Reads
// the guest object straight from RAM (big-endian bytes via sb_ram_fast), object-
// model — no GX byte-stream decode.
int capture_material() {
    g_cur_chan = ChanInfo{};                           // reset; stays invalid if no material
    const u32 matpacket = r32(J3DSYS + 0x3C);          // j3dSys.mMatPacket
    if (!valid(matpacket)) { g_mat_none++; return -1; }
    const u32 material = r32(matpacket + 0x38);        // J3DMatPacket::unk38
    if (!valid(material)) { g_mat_none++; return -1; }
    capture_colorchan(material);                       // N6: colour-channel/lighting state
    const u32 tevblock = r32(material + 0x28);         // J3DMaterial::mTevBlock
    if (!valid(tevblock)) { g_mat_none++; return -1; }
    const u32 vt = r32(tevblock + 0x00);               // J3DTevBlock vtable ptr

    // Vtable histogram (verification) — record up to 8 distinct values.
    for (int i = 0; i < 8; i++) {
        if (g_vt_hist_key[i] == vt) { g_vt_hist_cnt[i]++; break; }
        if (g_vt_hist_key[i] == 0) { g_vt_hist_key[i] = vt; g_vt_hist_cnt[i] = 1; break; }
    }

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

    // FNV-1a key over the captured state (excluding the key field itself).
    uint64_t h = 1469598103934665603ull;
    const u8* p = (const u8*)&st;
    for (size_t i = 0; i < offsetof(NgxTevState, key); i++) { h ^= p[i]; h *= 1099511628211ull; }
    st.key = h;

    if (ns <= 16) g_stage_hist[ns]++;
    g_mat_found++;

    auto it = g_tevkey_index.find(h);
    if (it != g_tevkey_index.end()) return it->second;
    if (g_tevstates.size() >= TEVSTATE_CAP) { g_tevstates.clear(); g_tevkey_index.clear(); }
    const int idx = (int)g_tevstates.size();
    g_tevstates.push_back(st);
    g_tevkey_index[h] = idx;
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
    for (size_t vi = 0; vi < nv; vi++) {
        const NgxVertex& v = g_verts[vi];
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
        if (g_snap.size() < SNAP_CAP) g_snap.resize(SNAP_CAP);
        // Per-texmap bindings for this shape, from the live g_curtex[8] table. CI
        // formats (C4/C8/C14X2) are OK iff their TLUT was resolved; else → none.
        NgxTexBind tb[8] = {};
        for (int m = 0; m < 8; m++) {
            const CurTex& c = g_curtex[m];
            const bool is_ci = (c.fmt == 0x8 || c.fmt == 0x9 || c.fmt == 0xA);
            if (c.valid && c.addr && (!is_ci || c.tlut_addr)) {
                tb[m] = NgxTexBind{c.addr, c.w, c.h, c.fmt, c.tlut_fmt, is_ci ? c.tlut_addr : 0u};
            }
        }
        for (size_t t = 0; t + 3 <= g_indices.size(); t += 3) {
            if (g_snap_count + 3 > SNAP_CAP) { g_snap_count = 0; g_batches.clear(); }
            // Open a new batch on material/binding change, after a wrap, or at start.
            const bool tex_diff = g_batches.empty() ||
                memcmp(g_batches.back().tex, tb, sizeof tb) != 0;
            if (g_batches.empty() || tex_diff ||
                g_batches.back().tev_index != g_cur_tev_index ||
                g_batches.back().vstart + g_batches.back().vcount != (uint32_t)g_snap_count) {
                if (g_batches.size() >= BATCH_CAP) break;  // bounded
                NgxRenderBatch nb{}; memcpy(nb.tex, tb, sizeof tb);
                nb.vstart = (uint32_t)g_snap_count; nb.vcount = 0; nb.tev_index = g_cur_tev_index;
                g_batches.push_back(nb);
            }
            for (int e = 0; e < 3; e++) {
                const unsigned vidx = g_indices[t + e];
                const float* cp = &g_clip[vidx * 4];
                const NgxVertex& v = g_verts[vidx];
                const float* lit = &g_litrgba[vidx * 4];
                NgxRenderVertex& d = g_snap[g_snap_count];
                d.clip[0]=cp[0]; d.clip[1]=cp[1]; d.clip[2]=cp[2]; d.clip[3]=cp[3];
                d.rgba[0]=lit[0]; d.rgba[1]=lit[1]; d.rgba[2]=lit[2]; d.rgba[3]=lit[3];
                d.uv[0]=v.tex[0][0]; d.uv[1]=v.tex[0][1];
                g_snap_count++;
            }
            g_batches.back().vcount += 3;
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

// Best-effort snapshot accessors for the native Vulkan mesh render (copy promptly
// — the emu thread keeps writing; a torn read at worst yields a stray triangle).
const NgxRenderVertex* ngx_snap_verts(int* nverts) {
    *nverts = (int)g_snap_count;
    return g_snap.empty() ? nullptr : g_snap.data();
}
const NgxRenderBatch* ngx_snap_batches(int* nbatches) {
    *nbatches = (int)g_batches.size();
    return g_batches.empty() ? nullptr : g_batches.data();
}
const NgxTevState* ngx_snap_tevstates(int* nstates) {
    *nstates = (int)g_tevstates.size();
    return g_tevstates.empty() ? nullptr : g_tevstates.data();
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
        if (id < 8 && valid(cpu.gpr[3])) decode_texobj(cpu.gpr[3], g_curtex[id]);
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
        "  native projection: have_proj=%d  clip.w>0=%lu/%lu  NDC xy in [-1,1]=%lu (%.1f%%)\n",
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
        g_ndc_inbox, g_ndc_total ? 100.0 * (double)g_ndc_inbox / (double)g_ndc_total : 0.0);

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
    n += snprintf(out + n, cap - n, "    GXLoadTexObj by texmap:");
    for (int i = 0; i < 9; i++) if (g_texobj_hist[i]) n += snprintf(out + n, cap - n, " [%d]=%lu", i, g_texobj_hist[i]);
    n += snprintf(out + n, cap - n, "   Preloaded:");
    for (int i = 0; i < 9; i++) if (g_preload_hist[i]) n += snprintf(out + n, cap - n, " [%d]=%lu", i, g_preload_hist[i]);
    n += snprintf(out + n, cap - n, "\n");
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
    return n;
}
