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
CurTex g_curtex;

// TLUT registry: tlut_name (GXTlut, the TMEM slot) → palette guest addr + format,
// populated by the GXLoadTlut tee. CI texobjs reference a tlut_name (texobj+0x18).
struct TlutEntry { u32 addr = 0; u8 fmt = 0; bool valid = false; };
TlutEntry g_tlut[256];

inline bool valid(u32 a) { return a >= 0x80000000u && a < 0x81800000u; }
inline u32  r32(u32 a) { return valid(a) ? mem_r32(a) : 0; }
inline u8   rb(u32 a) { return valid(a) ? (u8)(mem_r32(a) >> 24) : 0; }  // big-endian byte @ word
inline float rf(u32 a) { u32 u = r32(a); float f; std::memcpy(&f, &u, 4); return f; }

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

// Read the current material's TEV block into an NgxTevState, dedupe it into the
// table, and return its index (-1 if no material / unknown block variant). Reads
// the guest object straight from RAM (big-endian bytes via sb_ram_fast), object-
// model — no GX byte-stream decode.
int capture_material() {
    const u32 matpacket = r32(J3DSYS + 0x3C);          // j3dSys.mMatPacket
    if (!valid(matpacket)) { g_mat_none++; return -1; }
    const u32 material = r32(matpacket + 0x38);        // J3DMatPacket::unk38
    if (!valid(material)) { g_mat_none++; return -1; }
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
    bool first = (g_xf_total == 0);
    const size_t nv = g_verts.size();
    if (g_have_proj) g_clip.assign(nv * 4, 0.0f);
    for (size_t vi = 0; vi < nv; vi++) {
        const NgxVertex& v = g_verts[vi];
        const float x = v.pos[0], y = v.pos[1], z = v.pos[2];
        const float ex = m[0]*x + m[1]*y + m[2]*z  + m[3];
        const float ey = m[4]*x + m[5]*y + m[6]*z  + m[7];
        const float ez = m[8]*x + m[9]*y + m[10]*z + m[11];
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
        // Texmap-0 binding for this shape. CI formats (C4/C8/C14X2) are now OK iff
        // their TLUT was resolved; otherwise render flat (tex_addr 0).
        const bool is_ci = (g_curtex.fmt == 0x8 || g_curtex.fmt == 0x9 || g_curtex.fmt == 0xA);
        const bool tex_ok = g_curtex.valid && g_curtex.addr && (!is_ci || g_curtex.tlut_addr);
        const uint32_t taddr = tex_ok ? g_curtex.addr : 0u;
        for (size_t t = 0; t + 3 <= g_indices.size(); t += 3) {
            if (g_snap_count + 3 > SNAP_CAP) { g_snap_count = 0; g_batches.clear(); }
            // Open a new batch on texture/material change, after a wrap, or at start.
            if (g_batches.empty() || g_batches.back().tex_addr != taddr ||
                g_batches.back().tev_index != g_cur_tev_index ||
                g_batches.back().vstart + g_batches.back().vcount != (uint32_t)g_snap_count) {
                if (g_batches.size() >= BATCH_CAP) break;  // bounded
                g_batches.push_back(NgxRenderBatch{taddr, g_curtex.w, g_curtex.h, g_curtex.fmt,
                                                   tex_ok ? g_curtex.tlut_addr : 0u, g_curtex.tlut_fmt,
                                                   (uint32_t)g_snap_count, 0, g_cur_tev_index});
            }
            for (int e = 0; e < 3; e++) {
                const unsigned vidx = g_indices[t + e];
                const float* cp = &g_clip[vidx * 4];
                const NgxVertex& v = g_verts[vidx];
                NgxRenderVertex& d = g_snap[g_snap_count];
                d.clip[0]=cp[0]; d.clip[1]=cp[1]; d.clip[2]=cp[2]; d.clip[3]=cp[3];
                d.rgba[0]=v.clr[0][0]/255.f; d.rgba[1]=v.clr[0][1]/255.f;
                d.rgba[2]=v.clr[0][2]/255.f; d.rgba[3]=v.clr[0][3]/255.f;
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

SUNBRIGHT_OVERRIDE(ov_gxloadtexobj, 0x80360160u) {
    if (g_enabled && cpu.gpr[4] == 0) {          // GX_TEXMAP0
        const u32 obj = cpu.gpr[3];
        if (valid(obj)) {
            const u32 image0 = r32(obj + 0x08), image3 = r32(obj + 0x0C);
            g_curtex.w = (u16)((image0 & 0x3FF) + 1);
            g_curtex.h = (u16)(((image0 >> 10) & 0x3FF) + 1);
            g_curtex.fmt = (u8)((image0 >> 20) & 0xF);
            g_curtex.addr = 0x80000000u | ((image3 & 0x1FFFFFu) << 5);
            g_curtex.tlut_addr = 0; g_curtex.tlut_fmt = 0;
            if (g_curtex.fmt == 0x8 || g_curtex.fmt == 0x9 || g_curtex.fmt == 0xA) {  // CI → resolve TLUT
                const u32 name = r32(obj + 0x18) & 0xFF;   // texobj.tlutName
                if (g_tlut[name].valid) { g_curtex.tlut_addr = g_tlut[name].addr; g_curtex.tlut_fmt = g_tlut[name].fmt; }
            }
            g_curtex.valid = true;
        }
    }
    if (RecompFunc o = recomp_raw(0x80360160u)) o(cpu); else call_ppc(cpu, cpu.lr);
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
