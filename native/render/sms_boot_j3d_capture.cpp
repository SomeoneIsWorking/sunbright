// sms_boot_j3d_capture.cpp — the native scene-geometry capture (ONE owned path).
//
// In sms-boot the renderer is PC-native and fully owned: native/src/scene_drive.cpp drives the
// real GC draw flow (TSmJ3DScn::perform(8)) each frame, which runs entry()+viewCalc on only the
// ACTIVE scene models and draws the draw buffers. The draw bottoms out in J3DShape::draw(), whose
// GX issue is a no-op natively — so this TU taps J3DShape::draw and captures the shape's geometry
// into a frame-global NDC triangle buffer that the present hook (sms_boot_present.cpp) drains and
// rasterizes via nvk. There is no env gate and no second path: every active shape the engine draws
// is captured here, transformed by its faithful per-vertex draw matrix + the camera projection.
//
// ENDIANNESS (the #1 risk): the ngx vertex/DL decoders read BIG-ENDIAN. In the live host BMD
// buffer the SHP1 display-list stays BE (bmd_swap defers it) and swap_VTX1 leaves the vertex
// arrays BE too (the ngx contract) — so the decoder reads both directly. If geometry comes back
// as garbage (positions outside the bbox / NaN), the swap_VTX1 BE contract was violated upstream;
// fix it at the source, do not re-swap here (that would fork the decoder and hide the bug).

#include <JSystem/J3D/J3DGraphBase/J3DShape.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DVertex.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>   // j3dSys.getModel() — the active model being drawn
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>      // J3DModel + J3DModelData

#include "nvk.h"            // NvkVertex
#include "gx_imm_xform.h"   // SbImmRawVtx / SbImmVtx / imm_project (model->view->proj->NDC)
#include "ngx_mesh.h"       // NgxCP, NgxVertex, ngx_build_mesh

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace sb::render;

// Live GX projection (type + 6 packed floats) + viewport for imm_project. The perspective is set
// by GXSetProjection in the camera perform (runs before scene_drive's draw each frame).
extern "C" void sb_gx_get_projection(int* type, float proj[6], float vp[6]);

// Host-allocation gate (JKRHeap.cpp): the capture's std::vectors are HOST renderer data; routing
// them to host malloc keeps them off the game's JKR heap. (Memory is now bounded — tag+free — so
// this is hygiene, not the OOM fix.) RAII guard below.
extern "C" void sb_host_alloc_push(void);
extern "C" void sb_host_alloc_pop(void);

namespace {

struct HostAllocScope { HostAllocScope() { sb_host_alloc_push(); } ~HostAllocScope() { sb_host_alloc_pop(); } };

bool dbg_enabled() {
    static int v = -1;
    if (v < 0) { const char* e = std::getenv("SB_J3D_DBG"); v = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return v != 0;
}

// Frame-global capture buffer of transformed NDC triangle verts (3N). Clear-on-first-append-
// after-consume (the present drains via _take, which sets g_consumed; the next append clears).
std::vector<NvkVertex> g_tris;
bool g_consumed = true;

// ---- build_native_cp: reads NATIVE J3DShape + J3DVertexData struct fields (host-endian, 64-bit
// ptrs). Array bases become OFFSETS relative to `base` (the model buffer start) for the BE resolver.
bool build_native_cp(J3DShape* shape, J3DVertexData& vd, const uint8_t* base, NgxCP& cp) {
    GXVtxDescList* desc = shape->getVtxDesc();
    GXVtxAttrFmtList* fmt = vd.getVtxAttrFmtList();
    if (!desc || !fmt) return false;

    for (GXVtxDescList* e = desc; e->attr != 0xFF; ++e) {
        u32 attr = e->attr, type = e->type;
        if (attr == 0)                     cp.vcd_lo |= (type ? 1u : 0u) << 0;
        else if (attr <= 8)                cp.vcd_lo |= (type ? 1u : 0u) << attr;
        else if (attr == 9)                cp.vcd_lo |= (type & 3) << 9;    // POS
        else if (attr == 10)               cp.vcd_lo |= (type & 3) << 11;   // NRM
        else if (attr == 11)               cp.vcd_lo |= (type & 3) << 13;   // CLR0
        else if (attr == 12)               cp.vcd_lo |= (type & 3) << 15;   // CLR1
        else if (attr >= 13 && attr <= 20) cp.vcd_hi |= (type & 3) << (2 * (attr - 13));
    }

    u32 pos_type = 4, nrm_type = 4, tex_type[8] = {4,4,4,4,4,4,4,4};
    u32& A = cp.vat[0][0]; u32& B = cp.vat[0][1]; u32& C = cp.vat[0][2];
    for (GXVtxAttrFmtList* e = fmt; e->attr != 0xFF; ++e) {
        u32 attr = e->attr, cnt = e->cnt, type = e->type, frac = e->frac & 0x1f;
        switch (attr) {
        case 9:  A |= (cnt & 1) | ((type & 7) << 1) | (frac << 4); pos_type = type; break;
        case 10: A |= ((cnt ? 1u : 0u) << 9) | ((type & 7) << 10); if (cnt == 2) A |= 1u << 31;
                 nrm_type = type; break;
        case 11: A |= ((cnt & 1) << 13) | ((type & 7) << 14); break;
        case 12: A |= ((cnt & 1) << 17) | ((type & 7) << 18); break;
        case 13: A |= ((cnt & 1) << 21) | ((type & 7) << 22) | (frac << 25); tex_type[0]=type; break;
        case 14: B |= (cnt & 1) | ((type & 7) << 1) | (frac << 4);           tex_type[1]=type; break;
        case 15: B |= ((cnt & 1) << 9)  | ((type & 7) << 10) | (frac << 13); tex_type[2]=type; break;
        case 16: B |= ((cnt & 1) << 18) | ((type & 7) << 19) | (frac << 22); tex_type[3]=type; break;
        case 17: B |= ((cnt & 1) << 27) | ((type & 7) << 28); C |= frac;     tex_type[4]=type; break;
        case 18: C |= ((cnt & 1) << 5)  | ((type & 7) << 6)  | (frac << 9);  tex_type[5]=type; break;
        case 19: C |= ((cnt & 1) << 14) | ((type & 7) << 15) | (frac << 18); tex_type[6]=type; break;
        case 20: C |= ((cnt & 1) << 23) | ((type & 7) << 24) | (frac << 27); tex_type[7]=type; break;
        default: break;
        }
    }

    auto off = [&](const void* p) -> u32 { return p ? (u32)((const uint8_t*)p - base) : 0; };
    cp.array_base[0] = off(vd.getVtxPosArray());     cp.array_stride[0] = (pos_type == 4) ? 12 : 6;
    cp.array_base[1] = off(vd.getVtxNormArray());    cp.array_stride[1] = (nrm_type == 4) ? 12 : 6;
    cp.array_base[2] = off(vd.getVtxColorArray(0));  cp.array_stride[2] = 4;
    cp.array_base[3] = off(vd.getVtxColorArray(1));  cp.array_stride[3] = 4;
    for (int i = 0; i < 8; ++i) {
        cp.array_base[4 + i]   = off(vd.getVtxTexCoordArray(i));
        cp.array_stride[4 + i] = (tex_type[i] == 4) ? 8 : 4;
    }
    return true;
}

// Resolver: array_base holds an offset into the model buffer (relative to `base`); the array bytes
// are BIG-ENDIAN — ngx byteswaps them on read.
struct ResolveCtx { const uint8_t* base; size_t size; };
const unsigned char* resolve_native(unsigned off, void* user) {
    auto* c = (ResolveCtx*)user;
    if (off == 0 || (c->size && off >= c->size)) return nullptr;
    return c->base + off;
}

} // namespace

// ============================ THE CAPTURE (single owned path) =============================
// Tapped at the top of J3DShape::draw() during the native-driven scene draw (scene_drive.cpp).
// The shape is an ACTIVE, entry()'d shape; j3dSys.getModel() is its model (viewCalc'd this frame).
// Decode the shape geometry and transform each vertex by its faithful per-vertex draw matrix
// (model->getDrawMtx) -> the latched perspective 4x4 -> clip-space frustum clip -> Vulkan NDC.
// Returns true (captured) so J3DShape::draw skips the no-op GX issue.
extern "C" bool sb_boot_capture_j3d(J3DShape* shape) {
    if (!shape) return false;
    HostAllocScope _hostalloc;   // capture std::vectors -> host malloc, off the JKR heap

    J3DModel* model = j3dSys.getModel();
    if (!model) return true;
    J3DModelData* md = model->getModelData();
    if (!md) return true;

    J3DVertexData* vd = shape->unk44;
    if (!vd) return true;
    const uint8_t* base = (const uint8_t*)vd->getVtxPosArray();
    if (!base) return true;

    NgxCP cp{};
    if (!build_native_cp(shape, *vd, base, cp)) return true;

    // Decode every matrix group's DL into native verts + tri indices (BE DL stream).
    ResolveCtx rc{ base, 0 };
    std::vector<NgxVertex> verts;
    std::vector<unsigned> idx;
    for (u16 e = 0; e < shape->getMtxGroupNum(); ++e) {
        J3DShapeDraw* dp = shape->getShapeDraw(e);
        if (!dp || !dp->getDisplayList()) continue;
        ngx_build_mesh(cp, dp->getDisplayList(), dp->getDisplayListSize(),
                       resolve_native, &rc, verts, idx);
    }
    if (idx.empty()) return true;

    // Per-shape draw matrix (model->view): mDrawMatrices[viewNo][0] is the rigid matrix the
    // packet draw set up — the same one J3DShape::draw feeds j3dSys.setModelDrawMtx. imm_project
    // applies it then the live GX projection + viewport to reach Vulkan NDC (nvk does raster-time
    // clipping, so no clip-space stage needed here — the proven drive-path transform).
    int projType; float proj[6]; float vp[6];
    sb_gx_get_projection(&projType, proj, vp);
    float ident[3][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0}};
    float posMtx[3][4];
    Mtx* drawTbl = shape->mDrawMatrices ? shape->mDrawMatrices[*shape->mCurrentViewNo] : nullptr;
    if (drawTbl) std::memcpy(posMtx, &drawTbl[0][0], sizeof(posMtx));
    else         std::memcpy(posMtx, ident, sizeof(posMtx));

    if (g_consumed) { g_tris.clear(); g_consumed = false; }
    if (g_tris.size() > 6u * 1024 * 1024) return true;   // OOM guard for an undrained config
    g_tris.reserve(g_tris.size() + idx.size());

    static long s_shapes = 0, s_tris = 0;
    if (dbg_enabled()) { ++s_shapes; s_tris += (long)idx.size();
        if ((s_shapes % 4000) == 0)
            std::fprintf(stderr, "[j3dcap] shapes=%ld tris=%ld gtris=%zu\n", s_shapes, s_tris, g_tris.size()); }

    for (unsigned i : idx) {
        const NgxVertex& s = verts[i];
        SbImmRawVtx raw{ s.pos[0], s.pos[1], s.pos[2],
                         s.clr[0][0]/255.f, s.clr[0][1]/255.f, s.clr[0][2]/255.f, s.clr[0][3]/255.f };
        if ((s.clr[0][0]|s.clr[0][1]|s.clr[0][2]) == 0) { raw.r = raw.g = raw.b = 0.86f; }
        SbImmVtx p = imm_project(raw, projType, proj, posMtx, vp);
        g_tris.push_back(NvkVertex{ p.x, p.y, p.z, p.r, p.g, p.b, p.a });
    }
    return true;
}

// Present drains the frame's captured scene tris (mirrors sb_gx_imm_take's signature). Returns the
// vertex count (multiple of 3); marks the buffer consumed so the next append clears it.
extern "C" int sb_boot_capture_j3d_take(const NvkVertex** out) {
    if (out) *out = g_tris.empty() ? nullptr : g_tris.data();
    int n = (int)g_tris.size();
    g_consumed = true;
    return n;
}
