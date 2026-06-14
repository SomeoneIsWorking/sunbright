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
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr u32 J3DSYS = 0x804045DCu;

inline bool valid(u32 a) { return a >= 0x80000000u && a < 0x81800000u; }
inline u32  r32(u32 a) { return valid(a) ? mem_r32(a) : 0; }
inline u8   rb(u32 a) { return valid(a) ? (u8)(mem_r32(a) >> 24) : 0; }  // big-endian byte @ word
inline float rf(u32 a) { u32 u = r32(a); float f; std::memcpy(&f, &u, 4); return f; }

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
bool          g_enabled = false;
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
// a rolling window of recent scene triangles, each vertex = clip.xyzw (4f) +
// rgba0 (4f). The render reads this best-effort from the HTTP thread (a torn
// read at worst shows a stray triangle — diagnostic-acceptable).
constexpr size_t SNAP_CAP = 600000;   // vertices (200k tris) ≈ 19 MB
std::vector<float> g_snap;            // SNAP_CAP*8, lazily reserved
size_t             g_snap_count = 0;  // valid vertices currently in g_snap

// Reusable scratch (single emu/render thread serialized by nthr).
std::vector<NgxVertex> g_verts;
std::vector<unsigned>  g_indices;
std::vector<float>     g_clip;        // 4 floats/vertex (clip-space), scratch

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

    // Accumulate this shape's clip-space triangles into the render snapshot
    // (triangle-aligned roll-over so a wrap never splits a triangle).
    if (g_have_proj && !g_indices.empty()) {
        if (g_snap.size() < SNAP_CAP * 8) g_snap.resize(SNAP_CAP * 8);
        for (size_t t = 0; t + 3 <= g_indices.size(); t += 3) {
            if (g_snap_count + 3 > SNAP_CAP) g_snap_count = 0;
            for (int e = 0; e < 3; e++) {
                const unsigned vidx = g_indices[t + e];
                const float* cp = &g_clip[vidx * 4];
                const NgxVertex& v = g_verts[vidx];
                float* d = &g_snap[g_snap_count * 8];
                d[0]=cp[0]; d[1]=cp[1]; d[2]=cp[2]; d[3]=cp[3];
                d[4]=v.clr[0][0]/255.f; d[5]=v.clr[0][1]/255.f;
                d[6]=v.clr[0][2]/255.f; d[7]=v.clr[0][3]/255.f;
                g_snap_count++;
            }
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

// Best-effort snapshot of the accumulated clip-space triangle list for the native
// Vulkan mesh render. Returns the base pointer (8 floats/vertex: clip.xyzw +
// rgba0) and sets *nverts. Read from the HTTP thread; the emu thread keeps
// writing, so the caller should copy promptly (a torn read at worst yields a
// stray triangle — acceptable for a diagnostic render).
const float* ngx_mesh_snapshot(int* nverts) {
    *nverts = (int)g_snap_count;
    return g_snap.empty() ? nullptr : g_snap.data();
}

SUNBRIGHT_OVERRIDE(ov_j3dshape_draw, 0x802e0390u) {
    static const bool init = (g_enabled = (getenv("SUNBRIGHT_NGX_SHAPE") != nullptr), true);
    (void)init;
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
    return n;
}
