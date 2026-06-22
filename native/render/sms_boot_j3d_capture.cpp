// sms_boot_j3d_capture.cpp — SLICE 3a of attaching the native renderer to sms-boot.
//
// SLICE 1 rendered the clear colour; SLICE 2 added the immediate-mode 2D (fader/HUD).
// SLICE 3 captures the live 3D scene: the engine's J3D packet walk bottoms out in
// J3DShape::draw(), which in the native build issues GXCallDisplayList (a no-op) — so
// the 3D geometry currently goes nowhere and the frame is black behind the 2D.
//
// This TU provides the capture body sb_boot_capture_j3d(shape), called from a tiny
// SMS_NATIVE_PLATFORM hook at the top of J3DShape::draw() (reference/sms/.../J3DShape.cpp).
// It decodes ONE shape's geometry with the SAME proven recipe as native/render/tests/
// j3dmesh_test.cpp (build_native_cp + ngx_build_mesh, reading NATIVE host-endian struct
// fields), transforms each vertex to Vulkan NDC, and appends the triangles to a
// frame-global buffer. The present hook drains it via sb_boot_capture_j3d_take(), exactly
// mirroring sb_gx_imm_take() (sms_boot_present.cpp).
//
// ---------------------------------------------------------------------------
// 3a TRANSFORM (documented, deliberately simple):
//   The goal of 3a is to prove SOME 3D geometry reaches the frame (nonzero non-clear
//   pixels), NOT a correct camera. We therefore transform each shape with an
//   ORTHO / bounding-box fit over the shape's OWN bbox (J3DShape::unk10..unk1C), exactly
//   like j3dmesh_test's ortho fit (j3dmesh_test.cpp:275-285) — IGNORING the camera/draw
//   matrices entirely. This sidesteps the model-local-vs-model-view question (plan §5.2)
//   for the smallest verifiable step. 3c switches to imm_project() with the captured
//   projection/viewport + the per-shape draw matrix (the camera-correct path); the wiring
//   for that is present below behind SB_J3D_CAMERA=1 but is NOT the 3a default and is
//   UNVERIFIED — see the TODO at use_camera.
//
// ---------------------------------------------------------------------------
// ENDIANNESS (the #1 risk, plan §4) — DECISION, with evidence:
//   The ngx vertex/DL decoders read BIG-ENDIAN and byteswap on every read
//   (runtime/ngx/ngx_vertex.cpp read_comp/bef32; ngx_decode.cpp be16/be32). So the decoder
//   contract is: BOTH the DL stream AND the vertex arrays must be big-endian.
//
//   In the live sms-boot host BMD buffer (after bmd_swap_to_host):
//     * DL stream interior: bmd_swap DEFERS SHP1 display-list swapping → stays BIG-ENDIAN
//       (native/assets/bmd_swap.cpp swap_SHP1 note, lines 309-314). ✅ ngx reads it directly.
//     * Vertex arrays (VTX1): swap_VTX1 (native/assets/bmd_swap.cpp:280-291, swap_run over
//       each array region with the format-derived scalar unit) DOES byteswap the array DATA
//       to HOST-ENDIAN. ✗ This CONTRADICTS J3DModelLoader.cpp:30-32's comment ("deliberately
//       leaves ... vertex-array ... byte-streams big-endian") — the comment is aspirational;
//       the code swaps. ngx would then decode host-endian arrays through a BE reader = garbage.
//
//   ROOT-CAUSE FIX (plan §4 fix B-1, applied by the parent as a reviewed shipping edit):
//   make swap_VTX1 match its own contract — leave the VTX1 array DATA big-endian (keep the
//   structural offset + fmt-list swaps; SKIP the swap_run over the array regions). This is
//   safe: native GX is stubbed, so NOTHING reads array scalars host-endian — only the
//   structural fields (which stay swapped). After that edit, the live arrays are BE and this
//   capture works directly, just like j3dmesh_test (which keeps its own BE copy).
//
//   This TU is written for the POST-FIX world (arrays BE). If SB_J3D_CAPTURE shows garbage
//   geometry (positions far outside the shape bbox / NaN), the swap_VTX1 edit was not applied
//   — that is the gate, not a bug here. (We do NOT re-swap arrays back to BE on the fly here:
//   that would fork the decoder and hide the real contract violation — a bandaid. Fix it at
//   the source.)

#include <JSystem/J3D/J3DGraphBase/J3DShape.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DVertex.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>   // j3dSys (live array-base overrides + view mtx)
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>      // J3DModel + J3DModelData (shape enum)

#include "nvk.h"            // NvkVertex
#include "gx_imm_xform.h"   // SbImmRawVtx (kept for type)
#include "ngx_mesh.h"       // NgxCP, NgxVertex, ngx_build_mesh
#include "ngx_project.h"    // ngx_project_eye (eye -> clip via the 4x4)
#include "ngx_clip.h"       // ngx_clip_frustum_tri (clip-space frustum clip)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace sb::render;

// Bridge into native gx_impl (added there, mirrors sb_gx_get_clear_color): the captured
// projection + viewport, layout exactly as imm_project() wants.
//   type: 0 = GX_PERSPECTIVE, 1 = GX_ORTHOGRAPHIC
//   proj[0..5] = GXState.projMtx  (== GXGetProjectionv ptr[1..6])
//   vp[0..5]   = {left, top, width, height, nearz, farz}
extern "C" void sb_gx_get_projection(int* type, float proj[6], float vp[6]);
extern "C" int  sb_gx_get_proj44(float m[16]);   // latched perspective 4x4 (row-major)
extern "C" void sb_boot_latch_camera();          // own the camera: gpCamera -> j3dSys view + proj latch

// Host-allocation gate (JKRHeap.cpp): while raised, plain operator new routes to host
// malloc instead of the game's JKR heap. The capture's std::vectors (g_tris + per-shape
// verts/idx) are HOST renderer data — without this gate they drain the game heap every
// frame and OOM it at scene setup (the JKRHeap:694 abort). RAII guard below.
extern "C" void sb_host_alloc_push(void);
extern "C" void sb_host_alloc_pop(void);

namespace {

struct HostAllocScope { HostAllocScope() { sb_host_alloc_push(); } ~HostAllocScope() { sb_host_alloc_pop(); } };


bool capture_enabled() {
    static int v = -1;
    if (v < 0) { const char* e = std::getenv("SB_J3D_CAPTURE"); v = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return v != 0;
}
// 3a default = ortho bbox fit (camera-independent). Opt in to the (unverified) camera path
// with SB_J3D_CAMERA=1 once 3a is proven; see the TODO below.
bool use_camera() {
    static int v = -1;
    if (v < 0) { const char* e = std::getenv("SB_J3D_CAMERA"); v = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return v != 0;
}

// Frame-global capture buffer of transformed NDC triangle verts (3N). Clear-on-first-
// append-after-consume — the same latch as gx_imm_impl's g_consumed (sms_boot_present.cpp
// drains via _take, which sets g_consumed=true; the next append clears).
std::vector<NvkVertex> g_tris;
bool g_consumed = true;

// ---- build_native_cp: ported from native/render/tests/j3dmesh_test.cpp:76-132 ----------
// Reads NATIVE J3DShape + J3DVertexData struct fields (host-endian, 64-bit ptrs). Array
// bases become OFFSETS relative to `base` (the model buffer start) for the BE resolver.
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

// Resolver: array_base holds an offset into the model buffer (relative to `base`); the
// array bytes are BIG-ENDIAN (post swap_VTX1 fix) — ngx byteswaps them on read.
struct ResolveCtx { const uint8_t* base; size_t size; };
const unsigned char* resolve_native(unsigned off, void* user) {
    auto* c = (ResolveCtx*)user;
    if (off == 0 || (c->size && off >= c->size)) return nullptr;
    return c->base + off;
}

} // namespace

// ============================ THE CAPTURE BODY ===========================================
// Called from J3DShape::draw() under SMS_NATIVE_PLATFORM (the tap), BEFORE the no-op GX
// issue. At that point the shape packet has already populated mDrawMatrices/mCurrentViewNo
// (J3DShapePacket::draw, reference/sms/.../J3DPacket.cpp), and j3dSys holds the live
// POS/NRM/CLR0 array-base overrides + the view matrix.
//
// Returns true if it captured this shape (the caller may then skip the GX no-op — though
// skipping is moot since GXCallDisplayList does nothing). When SB_J3D_CAPTURE is off it
// early-returns false so a normal run pays nothing.
extern "C" bool sb_boot_capture_j3d(J3DShape* shape) {
    if (!capture_enabled() || !shape) return false;
    // DIAGNOSTIC (SB_J3D_DBG): count calls + early-return reasons so we can see whether the
    // J3DShape::draw tap fires at all and where shapes drop out. Remove once 3a is verified.
    static int dbg = -1;
    if (dbg < 0) { const char* e = std::getenv("SB_J3D_DBG"); dbg = (e && e[0] && e[0] != '0') ? 1 : 0; }
    static long s_calls = 0, s_novd = 0, s_nobase = 0, s_nocp = 0, s_noidx = 0, s_ok = 0, s_tris = 0;
    ++s_calls;
    if (dbg && (s_calls % 2000) == 0)
        std::fprintf(stderr, "[j3dcap] calls=%ld ok=%ld tris=%ld | drop: novd=%ld nobase=%ld nocp=%ld noidx=%ld\n",
                     s_calls, s_ok, s_tris, s_novd, s_nobase, s_nocp, s_noidx);
#define J3DCAP_DROP(ctr) do { ++(ctr); return true; } while (0)
    if (g_consumed) { g_tris.clear(); g_consumed = false; }
    // Safety cap: the present hook drains via _take() once per presented frame, but it only
    // runs while frame-dumping is active (sms_boot_present.cpp). If capture is enabled WITHOUT
    // dumping, _take is never called and the buffer would grow unbounded across frames. Cap it
    // so an undrained run can't OOM; the verify recipe pairs SB_J3D_CAPTURE with SB_FRAME_DUMP
    // (drained every frame), so this never triggers there. (Not a fidelity bandaid — purely an
    // out-of-memory guard for the undrained-config; 3b ties capture lifetime to the present.)
    if (g_tris.size() > 6u * 1024 * 1024) return true;

    J3DVertexData* vd = shape->unk44;   // == the shape's vertex data (public field)
    if (!vd) J3DCAP_DROP(s_novd);

    // Model-buffer base for the resolver. The decoded arrays live in the persisted host BMD
    // buffer; build_native_cp turns each native array pointer into an offset relative to
    // `base`, and resolve_native turns it back — so as long as `base` is a fixed anchor that
    // sits at or before every array, the round-trip is identity. We use the POS array start
    // as that anchor and size=0 (no upper bound) so any array AFTER pos resolves; an array
    // BEFORE pos would give a negative (wrapped) offset.
    // TODO(3b): thread the true J3DModelData buffer base (J3DModelData has the block base)
    // so arrays preceding POS also resolve; for 3a/most rigid plaza shapes POS is first.
    const uint8_t* base = (const uint8_t*)vd->getVtxPosArray();
    if (!base) J3DCAP_DROP(s_nobase);

    NgxCP cp{};
    if (!build_native_cp(shape, *vd, base, cp)) J3DCAP_DROP(s_nocp);

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
    if (idx.empty()) J3DCAP_DROP(s_noidx);
    ++s_ok; s_tris += (long)idx.size();

    // --- transform decoded model-space verts to Vulkan NDC ---
    g_tris.reserve(g_tris.size() + idx.size());

    if (use_camera()) {
        // 3c path (UNVERIFIED — opt-in via SB_J3D_CAMERA): real camera. Feed the per-shape
        // draw matrix as imm_project's current position matrix and the captured projection +
        // viewport. mDrawMatrices is Mtx** (Mtx = f32[3][4]); mDrawMatrices[viewNo] is Mtx*
        // (== f32(*)[4]) = the per-view draw-matrix table; [0] is the rigid (single-packet)
        // matrix — same one J3DShape::draw feeds j3dSys.setModelDrawMtx (J3DShape.cpp:230).
        // TODO(3c): (a) honor per-vertex matidx/packet for skinned shapes; (b) if geometry is
        // DOUBLY-transformed (draw matrix is model-LOCAL not model-view), left-multiply
        // j3dSys.getViewMtx() — the #1 thing to eyeball here (plan §5.2).
        int projType; float proj[6]; float vp[6];
        sb_gx_get_projection(&projType, proj, vp);
        float ident[3][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0}};
        float posMtx[3][4];
        Mtx* drawTbl = shape->mDrawMatrices ? shape->mDrawMatrices[*shape->mCurrentViewNo] : nullptr;
        if (drawTbl) std::memcpy(posMtx, &drawTbl[0][0], sizeof(posMtx));
        else         std::memcpy(posMtx, ident, sizeof(posMtx));

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

    // 3a DEFAULT: ortho bbox fit over the shape's OWN bbox (unk10..unk1C), camera-ignored
    // (j3dmesh_test.cpp:275-285). Proves the geometry decodes + rasterizes without depending
    // on any matrix being correct. Maps model XY into [-1,1] NDC (Vulkan +Y down → flip Y so
    // the model isn't upside down); z is set to 0.5 (mid-depth) since 3a has no depth test.
    const Vec& mn = shape->unk10; const Vec& mx = shape->unk1C;
    const float cx = (mn.x + mx.x) * 0.5f, cy = (mn.y + mx.y) * 0.5f;
    float ex = std::max(mx.x - mn.x, mx.y - mn.y) * 0.55f;
    if (!(ex > 0.f)) ex = 1.f;   // degenerate/zero bbox guard (also catches NaN)

    for (unsigned i : idx) {
        const NgxVertex& s = verts[i];
        float nx = (s.pos[0] - cx) / ex;
        float ny = -(s.pos[1] - cy) / ex;   // Vulkan NDC y is DOWN
        unsigned char r = s.clr[0][0], g = s.clr[0][1], b = s.clr[0][2];
        if ((r | g | b) == 0) { r = g = b = 220; }   // no vertex colour → light grey
        g_tris.push_back(NvkVertex{ nx, ny, 0.5f, r/255.f, g/255.f, b/255.f, 1.f });
    }
    return true;
}

// ============================ MODEL-LEVEL CAPTURE (PC-native draw) =======================
// Called from J3DModel::calc() under SMS_NATIVE_PLATFORM. In sms-boot the GC draw phase
// (camera + model->entry() + draw-buffer drawHead + J3DShape::draw + GXCallDisplayList)
// does NOT run — only the per-frame calc() does (verified: calc fires, entry never does).
// So instead of relying on that GC pipeline, we render each model DIRECTLY here, the
// PC-native way: run the view-matrix concat (viewCalc, using the camera view j3dSys already
// holds) to get each shape's MODEL->VIEW draw matrix, decode the shape geometry, and project
// to NDC with the latched PERSPECTIVE projection. This drops the entire GC draw-buffer/
// packet/display-list machinery. Gated on SB_J3D_CAPTURE.
//
// 3a scope: rigid shapes via draw-matrix 0 (correct for single-joint map pieces; multi-
// joint/skinned use the wrong matrix — refined later). Proves the scene reaches the frame.
extern "C" void sb_boot_capture_model(J3DModel* model) {
    if (!capture_enabled() || !model) return;
    HostAllocScope _hostalloc;   // all std::vector growth below is host-malloc, not JKR heap
    static int dbg = -1;
    if (dbg < 0) { const char* e = std::getenv("SB_J3D_DBG"); dbg = (e && e[0] && e[0] != '0') ? 1 : 0; }
    static long s_calls = 0, s_shapes = 0, s_tris = 0, s_nomd = 0, s_drop = 0;
    ++s_calls;
    if (dbg && (s_calls % 200) == 0)
        std::fprintf(stderr, "[capmodel] calls=%ld shapes=%ld tris=%ld | nomd=%ld dropshape=%ld gtris=%zu\n",
                     s_calls, s_shapes, s_tris, s_nomd, s_drop, g_tris.size());
    if (g_consumed) { g_tris.clear(); g_consumed = false; }
    if (g_tris.size() > 6u * 1024 * 1024) return;

    J3DModelData* md = model->getModelData();
    if (!md) { ++s_nomd; return; }

    // Own the camera: publish the active camera's view (into j3dSys) + perspective (latch)
    // before viewCalc reads them. The GC draw phase that would do this doesn't run in sms-boot.
    sb_boot_latch_camera();

    // Compute the view-space draw matrices for this frame (camera view from j3dSys).
    model->viewCalc();

    float P[16];
    int phave = sb_gx_get_proj44(P);   // latched perspective 4x4 (row-major); identity until first set

    const u16 drawMtxNum = md->getDrawMtxNum();

    // One-shot transform diagnostic: dump the projection 4x4, a real shape's first vertex
    // through the whole chain (model->eye->clip->ndc), and the emitted NDC bbox. This is the
    // measurement, not eyeballing the blob.
    static int g_diag = -1;
    if (g_diag < 0) { const char* e = std::getenv("SB_J3D_DBG"); g_diag = (e && e[0] && e[0] != '0') ? 1 : 0; }
    static long g_diag_done = 0;
    bool diag = g_diag && (g_diag_done < 3) && phave;   // only once a real perspective is latched
    float nbx0=1e30f, nby0=1e30f, nbx1=-1e30f, nby1=-1e30f, nbz0=1e30f, nbz1=-1e30f;
    if (diag) {
        std::fprintf(stderr, "[diag] phave=%d drawMtxNum=%d P=[%.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f / "
                     "%.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f]\n", phave, drawMtxNum,
                     P[0],P[1],P[2],P[3],P[4],P[5],P[6],P[7],P[8],P[9],P[10],P[11],P[12],P[13],P[14],P[15]);
        const float (*dm0)[4] = model->getDrawMtx(0);
        std::fprintf(stderr, "[diag] drawMtx0=[%.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f]\n",
                     dm0[0][0],dm0[0][1],dm0[0][2],dm0[0][3], dm0[1][0],dm0[1][1],dm0[1][2],dm0[1][3],
                     dm0[2][0],dm0[2][1],dm0[2][2],dm0[2][3]);
    }

    const u16 shapeNum = md->getShapeNum();
    for (u16 si = 0; si < shapeNum; ++si) {
        J3DShape* shape = md->getShapeNodePointer(si);
        if (!shape) { ++s_drop; continue; }
        J3DVertexData* vd = shape->unk44;
        if (!vd) { ++s_drop; continue; }
        const uint8_t* base = (const uint8_t*)vd->getVtxPosArray();
        if (!base) { ++s_drop; continue; }

        NgxCP cp{};
        if (!build_native_cp(shape, *vd, base, cp)) { ++s_drop; continue; }

        ResolveCtx rc{ base, 0 };
        std::vector<NgxVertex> verts;
        std::vector<unsigned> idx;
        for (u16 e = 0; e < shape->getMtxGroupNum(); ++e) {
            J3DShapeDraw* dp = shape->getShapeDraw(e);
            if (!dp || !dp->getDisplayList()) continue;
            ngx_build_mesh(cp, dp->getDisplayList(), dp->getDisplayListSize(),
                           resolve_native, &rc, verts, idx);
        }
        if (idx.empty()) { ++s_drop; continue; }
        ++s_shapes; s_tris += (long)idx.size();
        (void)nbx0;

        g_tris.reserve(g_tris.size() + idx.size());
        // Per TRIANGLE: transform each vertex model->eye (its faithful per-vertex draw
        // matrix) -> clip (the 4x4 projection), then clip the triangle against the frustum
        // in CLIP SPACE (ngx_clip_frustum_tri — without this, verts at/behind the near plane
        // make w~0 and the perspective divide explodes into screen-spanning spikes), then
        // perspective-divide the clipped polygon to Vulkan NDC and emit.
        for (size_t t = 0; t + 2 < idx.size(); t += 3) {
            float in[3][8];   // per clip vert: clip.xyzw + rgba (stride 8)
            for (int k = 0; k < 3; ++k) {
                const NgxVertex& s = verts[idx[t + k]];

                // Faithful per-vertex draw matrix (J3DShape::draw matrix pipeline): the
                // vertex's matidx is its GP pos-matrix slot (rows 0,3,6,… -> slot=matidx/3);
                // mMatrices[packet]->getUseMtxIndex(slot) is the model draw-matrix index (unk4
                // for a single/rigid shape, useMtxIndexTable[slot] for skinned). getDrawMtx()
                // is the view-space (model->eye) matrix viewCalc() built.
                const J3DShapeMtx* smtx =
                    (s.packet < shape->getMtxGroupNum()) ? shape->getShapeMtx(s.packet) : nullptr;
                u16 di = 0;
                if (smtx) {
                    u16 slot = (u16)(s.matidx / 3);
                    u16 ix = (slot < smtx->getUseMtxNum()) ? smtx->getUseMtxIndex(slot)
                                                           : smtx->getUseMtxIndex(0);
                    di = (ix != 0xffff && ix < drawMtxNum) ? ix : 0;
                }
                const float (*dm)[4] = model->getDrawMtx(di);   // 3x4 model->eye
                const float ex = dm[0][3] + dm[0][0]*s.pos[0] + dm[0][1]*s.pos[1] + dm[0][2]*s.pos[2];
                const float ey = dm[1][3] + dm[1][0]*s.pos[0] + dm[1][1]*s.pos[1] + dm[1][2]*s.pos[2];
                const float ez = dm[2][3] + dm[2][0]*s.pos[0] + dm[2][1]*s.pos[1] + dm[2][2]*s.pos[2];
                ngx_project_eye(P, ex, ey, ez, in[k]);   // -> clip.xyzw at in[k][0..3]
                if (diag && t == 0 && k == 0) {
                    float w = in[k][3];
                    std::fprintf(stderr, "[diag] v0 model=(%.1f %.1f %.1f) di=%u eye=(%.1f %.1f %.1f) "
                                 "clip=(%.2f %.2f %.2f %.2f) ndc=(%.3f %.3f)\n",
                                 s.pos[0],s.pos[1],s.pos[2], di, ex,ey,ez,
                                 in[k][0],in[k][1],in[k][2],in[k][3],
                                 w!=0?in[k][0]/w:0.f, w!=0?-(in[k][1]/w):0.f);
                }

                float r = s.clr[0][0]/255.f, g = s.clr[0][1]/255.f,
                      b = s.clr[0][2]/255.f, a = s.clr[0][3]/255.f;
                if ((s.clr[0][0]|s.clr[0][1]|s.clr[0][2]) == 0) { r = g = b = 0.80f; }
                in[k][4] = r; in[k][5] = g; in[k][6] = b; in[k][7] = a;
            }

            float poly[9][8];
            int np = ngx_clip_frustum_tri(&in[0][0], 8, &poly[0][0]);
            if (np < 3) continue;
            for (int f = 0; f < np - 2; ++f) {
                const float* tri[3] = { poly[0], poly[f + 1], poly[f + 2] };
                for (int k = 0; k < 3; ++k) {
                    const float* v = tri[k];
                    float w = v[3]; if (w < 1e-6f) w = 1e-6f;
                    // GC clip -> Vulkan NDC: x=cx/w, y FLIPPED (GC +Y up, Vulkan +Y down),
                    // z = cz/w + 1 (GC NDC z in [-1,0] -> Vulkan [0,1]).
                    float nx = v[0]/w, ny = -(v[1]/w), nz = v[2]/w + 1.0f;
                    if (diag) { if(nx<nbx0)nbx0=nx; if(nx>nbx1)nbx1=nx; if(ny<nby0)nby0=ny;
                                if(ny>nby1)nby1=ny; if(nz<nbz0)nbz0=nz; if(nz>nbz1)nbz1=nz; }
                    g_tris.push_back(NvkVertex{ nx, ny, nz, v[4], v[5], v[6], v[7] });
                }
            }
        }
    }
    if (diag) {
        std::fprintf(stderr, "[diag] emitted NDC bbox x[%.2f,%.2f] y[%.2f,%.2f] z[%.2f,%.2f] tris=%zu\n",
                     nbx0, nbx1, nby0, nby1, nbz0, nbz1, g_tris.size()/3);
        ++g_diag_done;
    }
}

// Present drains the frame's captured scene tris (mirrors sb_gx_imm_take's signature).
// Returns the vertex count (multiple of 3); marks the buffer consumed so the next append
// (next frame's first shape) clears it.
extern "C" int sb_boot_capture_j3d_take(const NvkVertex** out) {
    if (out) *out = g_tris.empty() ? nullptr : g_tris.data();
    int n = (int)g_tris.size();
    g_consumed = true;
    return n;
}
